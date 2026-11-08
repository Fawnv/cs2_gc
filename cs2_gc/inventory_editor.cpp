#include "stdafx.h"
#include "inventory_editor.h"
#include "config.h"
#include "item_schema.h"
#include "message.h"
#include "keyvalue.h" // for FromString...
#include "mod_loader.h"

#include <libwebsockets.h>

constexpr uint32_t ProtocolVersion = 1;

// FIXME: is 4096 fair??+
constexpr size_t RxBufferSize = 4096;

enum CmdType : uint32_t
{
    // bidirectional
    CmdItemCreate, // item payload
    CmdItemModify, // item payload
    CmdItemDestroy, // low id only

    // sent as a response to CmdItemCreate by the GC
    // so the inventory editor can set the correct id
    CmdItemSetId, // uint32 (create request id), uint32 (created item id)
    CmdInventorySnapshot, // full inventory snapshot follows, editor should discard the old one
    CmdProtocolInfo, // uint32 ProtocolVersion

    // CSForge's Mods tab (strings are uint32 length + utf-8). Editors that never ask don't get these.
    CmdModListRequest, // editor -> gc, nothing
    CmdModList, // gc -> editor: uint32 flags (1 "mods" on in config, 2 loaded this run), string message,
                // uint32 count, per mod: string folder, name, author, version, description, uint32 enabled,
                // int32 priority, uint32 files, uint32 flags (1 items, 2 localization, 4 changed since start),
                // uint32 lost files, uint32 listed, per listed: string path, string winning mod folder
    CmdModSet // editor -> gc: string folder, uint32 enabled, int32 priority; answered with CmdModList
};

static void WriteString(MessageWrite &message, std::string_view text)
{
    message.WriteUint32(static_cast<uint32_t>(text.size()));
    message.WriteData(text.data(), static_cast<uint32_t>(text.size()));
}

static std::string ReadString(MessageRead &message)
{
    uint32_t size = message.ReadUint32();
    if (size > 4096)
    {
        message.ReadData(SIZE_MAX); // marks the message invalid
        return {};
    }

    const void *data = message.ReadData(size);
    return data ? std::string{ static_cast<const char *>(data), size } : std::string{};
}

static void ParseItem(MessageRead &message, EditorItem &item, ItemSchema &itemSchema)
{
    item.lowId = message.ReadUint32();

    item.desc.inventory = message.ReadUint32();
    item.desc.defIndex = ToEnum<ItemDefIndex>(message.ReadUint32());
    item.desc.level = message.ReadUint32();
    item.desc.quality = ToEnum<Quality>(message.ReadUint32());
    item.desc.flags = message.ReadUint32();
    item.desc.origin = message.ReadUint32();
    item.desc.inUse = message.ReadUint32();
    item.desc.rarity = ToEnum<Rarity>(message.ReadUint32());

    uint32_t attributeCount = message.ReadUint32();
    item.desc.attributes.reserve(attributeCount);

    for (uint32_t i = 0; i < attributeCount; i++)
    {
        AttributeDefIndex defIndex = ToEnum<AttributeDefIndex>(message.ReadUint32());

        size_t valueSize = message.ReadUint32();
        const void *valueData = message.ReadData(valueSize);
        if (!valueData)
        {
            // can't recover from this
            return;
        }

        std::string value{ static_cast<const char *>(valueData), valueSize };

        AttributeType type = itemSchema.GetAttributeType(defIndex);
        switch (type)
        {
        case AttributeType::Float:
            item.desc.attributes.emplace_back(defIndex, FromString<float>(value));
            break;

        case AttributeType::Uint32:
            item.desc.attributes.emplace_back(defIndex, FromString<uint32_t>(value));
            break;

        case AttributeType::String:
            item.desc.attributes.emplace_back(defIndex, std::move(value));
            break;
        }
    }

    uint32_t equipCount = message.ReadUint32();
    item.desc.equips.reserve(equipCount);

    for (uint32_t i = 0; i < equipCount; i++)
    {
        uint32_t classId = message.ReadUint32();
        uint32_t slotId = message.ReadUint32();
        item.desc.equips.emplace_back(classId, slotId);
    }
}

static void SerializeItem(MessageWrite &message, const EditorItem &item)
{
    message.WriteUint32(item.lowId);

    message.WriteUint32(item.desc.inventory);
    message.WriteUint32(FromEnum(item.desc.defIndex));
    message.WriteUint32(item.desc.level);
    message.WriteUint32(FromEnum(item.desc.quality));
    message.WriteUint32(item.desc.flags);
    message.WriteUint32(item.desc.origin);
    message.WriteUint32(item.desc.inUse);
    message.WriteUint32(FromEnum(item.desc.rarity));

    message.WriteUint32(static_cast<uint32_t>(item.desc.attributes.size()));
    for (const ItemAttribute &attribute : item.desc.attributes)
    {
        message.WriteUint32(FromEnum(attribute.DefIndex()));

        // illegible bullshit
        std::string value = std::visit(Bruh{
                                           [&](float v)
                                           { return FloatToString(v); },
                                           [&](uint32_t v)
                                           { return std::to_string(v); },
                                           [&](const std::string &v)
                                           { return v; },
                                       },
            attribute.Value());

        uint32_t valueSize = static_cast<uint32_t>(value.size());
        message.WriteUint32(valueSize);
        message.WriteData(value.data(), valueSize);
    }

    message.WriteUint32(static_cast<uint32_t>(item.desc.equips.size()));
    for (const ItemEquip &equip : item.desc.equips)
    {
        message.WriteUint32(equip.Class());
        message.WriteUint32(equip.Slot());
    }
}

InventoryEditor::InventoryEditor(ItemSchema &itemSchema)
    : m_itemSchema{ itemSchema }
{
    if (!GetConfig().m_inventoryEditor)
    {
        Platform::Print("Inventory editor server disabled\n");
        return;
    }

    lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);

    // need this due to the enum param (don't want to pollute includes with libwebsockets junk)
    auto callback = [](lws *wsi, lws_callback_reasons reason, void *user, void *in, size_t len)
    {
        return InventoryEditor::Callback(wsi, static_cast<int>(reason), user, in, len);
    };

    // FIXME: should actually be a member...
    static lws_protocols protocols[] = {
        { "default", callback, sizeof(Session), RxBufferSize, 0, nullptr, 0 },
        LWS_PROTOCOL_LIST_TERM
    };

    // cs2_gc ships the editor page itself (cs2_gc\editor, CS2 catalogue), served at
    // http://127.0.0.1:13001/ so no external site or extra web server is needed
    static std::string editorDir = Platform::DataPath("editor");
    static lws_http_mount mount{};
    mount.mountpoint = "/";
    mount.mountpoint_len = 1;
    mount.origin = editorDir.c_str();
    mount.def = "index.html";
    mount.origin_protocol = LWSMPRO_FILE;

    lws_context_creation_info info{};
    info.iface = "127.0.0.1";
    info.port = GetConfig().m_inventoryEditorPort;
    info.protocols = protocols;
    info.mounts = &mount;
    info.user = this;

    m_context = lws_create_context(&info);
    if (!m_context)
    {
        Platform::Print("Inventory editor server initialization failed\n");
        return;
    }

    Platform::Print("Inventory editor: open http://127.0.0.1:{}/ in a browser\n", GetConfig().m_inventoryEditorPort);
}

InventoryEditor::~InventoryEditor()
{
    if (m_context)
    {
        lws_context_destroy(m_context);
    }
}

bool InventoryEditor::GetChanges(EditorChanges &changes)
{
    if (!m_context)
    {
        return false;
    }

    lws_service(m_context, -1);

    if (m_inbox.empty())
    {
        return false;
    }

    bool succeeded = true;

    for (const Message &message : m_inbox)
    {
        if (!ParseMessage(message, changes))
        {
            succeeded = false;
            break;
        }
    }

    m_inbox.clear();

    if (changes.items.empty())
    {
        // shouldn't happen
        return false;
    }

    return succeeded;
}

void InventoryEditor::SendChanges(const EditorChanges &changes, bool isFull)
{
    if (!m_client || (!isFull && m_wantsFullInventory))
    {
        return;
    }

    Message message;
    if (SerializeMessage(changes, message, isFull))
    {
        m_outbox.push_back(std::move(message));
        lws_callback_on_writable(m_client);
        if (isFull)
        {
            m_wantsFullInventory = false;
        }
    }
}

bool InventoryEditor::ParseMessage(const Message &message, EditorChanges &changes)
{
    MessageRead read{ message.data(), static_cast<uint32_t>(message.size()) };

    while (!read.IsDone())
    {
        std::optional<EditorItem> item;

        uint32_t cmd = read.ReadUint32();
        switch (cmd)
        {
        case CmdItemCreate:
            item.emplace();
            item->type = EditorItemChangeType::Created;
            ParseItem(read, *item, m_itemSchema);
            break;

        case CmdItemModify:
            item.emplace();
            item->type = EditorItemChangeType::Modified;
            ParseItem(read, *item, m_itemSchema);
            break;

        case CmdItemDestroy:
            item.emplace();
            item->type = EditorItemChangeType::Destroyed;
            item->lowId = read.ReadUint32();
            break;

        case CmdModListRequest:
            QueueModList({});
            break;

        case CmdModSet:
        {
            std::string folder = ReadString(read);
            bool enabled = read.ReadUint32() != 0;
            int priority = static_cast<int32_t>(read.ReadUint32());
            if (!read.IsValid())
            {
                break;
            }

            std::string error;
            QueueModList(ModLoader::SetModState(folder, enabled, priority, error)
                    ? "Saved " + folder + ": " + (enabled ? "on" : "off") + ", priority " + std::to_string(priority) + ". Restart the game to apply."
                    : "Can't save " + folder + ": " + error);
            break;
        }

        default:
            Platform::Print("Unknown message type {} from item editor\n", cmd);
            return false;
        }

        if (!read.IsValid())
        {
            Platform::Print("Parsing message type {} from item editor failed\n", cmd);
            return false;
        }

        // looks good (mod commands have no item, they're answered right away)
        if (item)
        {
            changes.items.push_back(std::move(*item));
        }
    }

    return true;
}

void InventoryEditor::QueueModList(std::string_view message)
{
    if (!m_client)
    {
        return;
    }

    std::vector<ModLoader::ModStatus> mods = ModLoader::ModStatuses();

    MessageWrite write;
    write.WriteUint32(CmdModList);
    write.WriteUint32((GetConfig().m_mods ? 1u : 0u) | (ModLoader::LoadedThisRun() ? 2u : 0u));
    WriteString(write, message);
    write.WriteUint32(static_cast<uint32_t>(mods.size()));
    for (const ModLoader::ModStatus &mod : mods)
    {
        WriteString(write, mod.info.folder);
        WriteString(write, mod.info.name);
        WriteString(write, mod.info.author);
        WriteString(write, mod.info.version);
        WriteString(write, mod.info.description);
        write.WriteUint32(mod.info.enabled ? 1 : 0);
        write.WriteUint32(static_cast<uint32_t>(mod.info.priority));
        write.WriteUint32(static_cast<uint32_t>(mod.files));
        write.WriteUint32((mod.items ? 1u : 0u) | (mod.localization ? 2u : 0u) | (mod.changed ? 4u : 0u));
        write.WriteUint32(static_cast<uint32_t>(mod.lostFiles));
        write.WriteUint32(static_cast<uint32_t>(mod.lost.size()));
        for (const auto &[path, winner] : mod.lost)
        {
            WriteString(write, path);
            WriteString(write, winner);
        }
    }

    m_outbox.push_back(std::move(write).TakeBuffer());
    lws_callback_on_writable(m_client);
}

bool InventoryEditor::SerializeMessage(const EditorChanges &changes, Message &message, bool isFull)
{
    MessageWrite write;

    if (isFull)
    {
        write.WriteUint32(CmdInventorySnapshot);
    }

    for (const EditorItem &item : changes.items)
    {
        switch (item.type)
        {
        case EditorItemChangeType::Created:
            write.WriteUint32(CmdItemCreate);
            SerializeItem(write, item);
            break;

        case EditorItemChangeType::Modified:
            write.WriteUint32(CmdItemModify);
            SerializeItem(write, item);
            break;

        case EditorItemChangeType::Destroyed:
            write.WriteUint32(CmdItemDestroy);
            write.WriteUint32(item.lowId);
            break;

        default:
            assert(false);
            return false;
        }
    }

    message = std::move(write).TakeBuffer();
    return true;
}

void InventoryEditor::SendItemId(uint32_t requestId, uint32_t lowId)
{
    if (!m_client)
    {
        // we're cooked
        assert(false);
        return;
    }

    MessageWrite message;
    message.WriteUint32(CmdItemSetId);
    message.WriteUint32(requestId);
    message.WriteUint32(lowId);

    m_outbox.push_back(std::move(message).TakeBuffer());
    lws_callback_on_writable(m_client);
}

int InventoryEditor::Callback(lws *wsi, int reason, void *user, void *in, size_t len)
{
    auto self = static_cast<InventoryEditor *>(lws_context_user(lws_get_context(wsi)));

    switch (reason)
    {
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
    {
        // validate the origin here: our own page, or the configured external editor
        const std::string &expected = GetConfig().m_inventoryEditorOrigin;
        int length = lws_hdr_total_length(wsi, WSI_TOKEN_ORIGIN);
        if (length <= 0 || length > 256)
        {
            return 1;
        }

        std::string origin;
        origin.resize(static_cast<size_t>(length) + 1);
        if (lws_hdr_copy(wsi, origin.data(), length + 1, WSI_TOKEN_ORIGIN) != length)
        {
            return 1;
        }

        origin.resize(static_cast<size_t>(length));
        const std::string port = std::to_string(GetConfig().m_inventoryEditorPort);
        bool local = origin == "http://127.0.0.1:" + port || origin == "http://localhost:" + port;
        bool external = !expected.empty() && expected != "null" && origin == expected;
        if (!local && !external)
        {
            Platform::Print("Inventory editor origin mismatch ({}, expected {})\n", origin, expected);
            return 1;
        }

        // ok
        return 0;
    }

    case LWS_CALLBACK_ESTABLISHED:
    {
        // bruh
        new (user) Session{};

        // one client at a time
        lws *oldClient = self->m_client;
        self->m_client = wsi;
        if (oldClient && oldClient != wsi)
        {
            lws_set_timeout(oldClient, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
        }

        // i guess we wantt o clear these yeah
        self->m_inbox.clear();
        self->m_outbox.clear();

        // need to send the inventory
        self->m_wantsFullInventory = true;

        // let the editor know the protocol version we expect
        MessageWrite protocolInfo;
        protocolInfo.WriteUint32(CmdProtocolInfo);
        protocolInfo.WriteUint32(ProtocolVersion);
        self->m_outbox.push_back(std::move(protocolInfo).TakeBuffer());
        lws_callback_on_writable(wsi);
        break;
    }

    case LWS_CALLBACK_RECEIVE:
    {
        if (wsi != self->m_client)
        {
            break;
        }

        if (!lws_frame_is_binary(wsi))
        {
            lws_close_reason(wsi, LWS_CLOSE_STATUS_UNACCEPTABLE_OPCODE, nullptr, 0);
            return -1;
        }

        auto session = static_cast<Session *>(user);
        session->partial.insert(session->partial.end(), (uint8_t *)in, (uint8_t *)in + len);
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0)
        {
            self->m_inbox.push_back(std::move(session->partial));
            session->partial.clear();
        }

        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE:
    {
        if (wsi != self->m_client)
        {
            // this will close the connection
            return -1;
        }

        if (self->m_outbox.empty())
        {
            break;
        }

        Message &message = self->m_outbox.front();

        std::vector<uint8_t> temp(LWS_PRE + message.size());
        memcpy(temp.data() + LWS_PRE, message.data(), message.size());
        int written = lws_write(wsi, temp.data() + LWS_PRE, message.size(), LWS_WRITE_BINARY);
        if (written < 0 || static_cast<size_t>(written) < message.size())
        {
            // yikes, goodbye
            return -1;
        }

        self->m_outbox.pop_front();

        if (!self->m_outbox.empty())
        {
            // ask for more
            lws_callback_on_writable(wsi);
        }

        break;
    }

    case LWS_CALLBACK_CLOSED:
    {
        // FIXME: ugly
        auto session = static_cast<Session *>(user);
        session->~Session();

        // don't clobber the new client when the old one closes (schizo?)
        if (self->m_client == wsi)
        {
            self->m_client = nullptr;
            self->m_wantsFullInventory = false;
            self->m_inbox.clear();
            self->m_outbox.clear();
        }
        break;
    }

    default:
        // plain http (the editor page) is handled by the static mount
        return lws_callback_http_dummy(wsi, static_cast<lws_callback_reasons>(reason), user, in, len);
    }

    return 0;
}
