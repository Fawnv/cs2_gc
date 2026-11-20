import { Command, decodeMessage, encodeCreate, encodeDestroy, encodeModify, encodeModListRequest, encodeModSet,
  encodeModInstallBegin, encodeModInstallFile, encodeModInstallEnd, encodeModDelete } from './protocol.js';
import { openModArchive } from './mod-archive.js';

const INSTALL_PIECE = 256 * 1024; // the GC reads 64 KB at a time, a piece is a few reads
const INSTALL_BUFFERED = 4 * 1024 * 1024; // wait while the browser holds more than this unsent
import { InventoryState } from './inventory-state.js';

export class InventoryConnection {
  constructor(onChange, { WebSocketClass = globalThis.WebSocket, setTimer = globalThis.setTimeout, clearTimer = globalThis.clearTimeout } = {}) {
    this.onChange = onChange;
    this.WebSocketClass = WebSocketClass;
    // Browser timer APIs require Window as their receiver, even when stored here.
    this.setTimer = setTimer.bind(globalThis);
    this.clearTimer = clearTimer.bind(globalThis);
    this.inventory = new InventoryState();
    this.socket = null;
    this.timer = null;
    this.status = 'Disconnected';
    this.message = '';
    this.nextRequestId = 1;
    this.pendingCreate = null;
    this.createTimer = null;
    this.creation = null;
    this.pendingDelete = null;
    this.deleteTimer = null;
    this.deletion = null;
    // CSForge: { modsOn, loaded, message, mods, general } from the last modList, null until the GC answers
    this.mods = null;
    this.modsPending = false;
    // a mod being installed: { folder, done, total, error }
    this.installing = null;
  }

  deleteMod(folder) {
    if (this.socket?.readyState !== 1 || this.modsPending || this.installing) return;
    this.modsPending = true;
    this.socket.send(encodeModDelete(folder));
    this.onChange(this);
  }

  // a mod archive (.zip with mod.txt) from a file input or a drop: unpacked here, sent file by file
  async installMod(file) {
    const socket = this.socket;
    if (socket?.readyState !== 1 || this.modsPending || this.installing) return;
    const note = message => {
      this.mods = { ...(this.mods || { modsOn: false, loaded: false, mods: [], general: [] }), message };
      this.onChange(this);
    };
    let archive;
    try {
      archive = await openModArchive(file);
    } catch (error) {
      note(`Can't install ${file.name}: ${error.message}`);
      return;
    }
    this.installing = { folder: archive.folder, done: 0, total: archive.totalBytes, error: null };
    this.onChange(this);
    try {
      socket.send(encodeModInstallBegin(archive.folder));
      for (const entry of archive.files) {
        const bytes = await entry.read();
        let at = 0;
        do {
          if (this.socket !== socket || socket.readyState !== 1) throw new Error('the connection to the GC was lost');
          if (this.installing.error) throw new Error(this.installing.error);
          while (socket.bufferedAmount > INSTALL_BUFFERED) await new Promise(resolve => this.setTimer(resolve, 20));
          const piece = bytes.subarray(at, at + INSTALL_PIECE);
          socket.send(encodeModInstallFile(entry.path, at === 0, piece));
          at += INSTALL_PIECE;
          this.installing.done += piece.byteLength;
          this.onChange(this);
        } while (at < bytes.byteLength);
      }
      this.modsPending = true;
      socket.send(encodeModInstallEnd(true));
    } catch (error) {
      if (this.socket === socket && socket.readyState === 1) socket.send(encodeModInstallEnd(false));
      if (!this.installing.error) note(`Can't install ${archive.folder}: ${error.message}`);
    } finally {
      this.installing = null;
      this.onChange(this);
    }
  }

  requestMods() {
    if (this.socket?.readyState !== 1) return;
    this.modsPending = true;
    this.socket.send(encodeModListRequest());
    this.onChange(this);
  }

  setMod(folder, enabled, priority) {
    if (this.socket?.readyState !== 1 || this.modsPending) return;
    const bytes = encodeModSet(folder, enabled, priority);
    this.modsPending = true;
    this.socket.send(bytes);
    this.onChange(this);
  }

  get canCreate() {
    return this.canMutate && this.nextRequestId <= 0xffffffff;
  }

  get canMutate() {
    return this.inventory.ready && this.socket?.readyState === 1 && !this.pendingCreate && !this.pendingDelete;
  }

  delete(highId, label) {
    if (!this.canMutate || !this.inventory.items.has(highId)) throw new Error('Deletion requires a synchronized connection, an existing item, and no pending request.');
    const bytes = encodeDestroy(highId);
    const pending = { highId, label };
    this.pendingDelete = pending;
    this.deletion = { ...pending, status: 'pending', message: `Deleting ${label}: item ${highId}…` };
    try { this.socket.send(bytes); }
    catch (error) {
      this.pendingDelete = null;
      this.deletion = { ...pending, status: 'failed', message: `Delete send failed: ${error.message}` };
      this.onChange(this);
      return;
    }
    this.deleteTimer = this.setTimer(() => {
      if (this.pendingDelete !== pending) return;
      this.deleteTimer = null;
      this.deletion = { ...pending, status: 'uncertain', message: `Deletion of ${label} is unconfirmed. Wait or reconnect to inspect the fresh inventory.` };
      this.onChange(this);
    }, 10000);
    this.onChange(this);
  }

  // the GC applies it to the item (equips stay), tells the game and sends the item back changed
  modify(item, label) {
    if (!this.canMutate || !this.inventory.items.has(item.highId)) throw new Error('Saving requires a synchronized connection and an existing item.');
    this.socket.send(encodeModify(structuredClone(item)));
    this.modification = { highId: item.highId, message: `Saved ${label}.` };
    this.onChange(this);
  }

  trackDeletion(commands) {
    const pending = this.pendingDelete;
    if (!pending || !commands.some(c => c.type === Command.destroy && c.highId === pending.highId) || this.inventory.items.has(pending.highId)) return;
    this.clearTimer(this.deleteTimer);
    this.deleteTimer = null;
    this.pendingDelete = null;
    this.deletion = { ...pending, status: 'confirmed', message: `Deleted ${pending.label}: item ${pending.highId}.` };
  }

  create(template, label) {
    if (!this.canCreate) throw new Error('Creation requires a synchronized open connection and no pending request.');
    const requestId = this.nextRequestId;
    const draft = structuredClone(template);
    const bytes = encodeCreate(draft, requestId);
    this.nextRequestId++;
    const pending = { requestId, label, assignedId: null, observedIds: new Set() };
    this.pendingCreate = pending;
    this.creation = { status: 'pending', requestId, label, message: `Creating ${label} (request ${requestId})…` };
    try {
      this.socket.send(bytes);
    } catch (error) {
      this.pendingCreate = null;
      this.creation = { status: 'failed', requestId, label, message: `Send failed: ${error.message}` };
      this.onChange(this);
      return;
    }
    this.createTimer = this.setTimer(() => {
      if (this.pendingCreate !== pending) return;
      this.createTimer = null;
      this.creation = { status: 'uncertain', requestId, label, message: `Creation of ${label} is unconfirmed. It may already exist. Wait for confirmation or reconnect and inspect the fresh inventory before creating again.` };
      this.onChange(this);
    }, 10000);
    this.onChange(this);
  }

  trackCreation(commands) {
    const pending = this.pendingCreate;
    if (!pending) return;
    for (const command of commands) {
      if (command.type === Command.setId && command.requestId === pending.requestId) pending.assignedId = command.highId;
      if (command.type === Command.create || command.type === Command.modify) pending.observedIds.add(command.item.highId);
    }
    if (pending.assignedId !== null && pending.observedIds.has(pending.assignedId) && this.inventory.items.has(pending.assignedId)) {
      this.clearTimer(this.createTimer);
      this.createTimer = null;
      this.creation = { status: 'confirmed', requestId: pending.requestId, label: pending.label, highId: pending.assignedId, message: `Created ${pending.label}: item ${pending.assignedId}.` };
      this.pendingCreate = null;
    }
  }

  publish(status, message = '') {
    this.status = status;
    this.message = message;
    this.onChange(this);
  }

  release() {
    this.clearTimer(this.deleteTimer);
    this.deleteTimer = null;
    if (this.pendingDelete) {
      this.deletion = { ...this.pendingDelete, status: 'unconfirmed', message: `Disconnected before confirming deletion of ${this.pendingDelete.label}. Inspect the fresh inventory after reconnecting; deletion will not be retried.` };
    }
    this.pendingDelete = null;
    if (this.deletion?.status === 'confirmed') this.deletion = null;
    this.clearTimer(this.timer);
    this.timer = null;
    this.clearTimer(this.createTimer);
    this.createTimer = null;
    if (this.pendingCreate) {
      this.creation = { status: 'unconfirmed', requestId: this.pendingCreate.requestId, label: this.pendingCreate.label, message: `Disconnected before confirming ${this.pendingCreate.label}. The item may already exist. Inspect the fresh inventory after reconnecting; this request will not be retried.` };
    }
    this.pendingCreate = null;
    if (this.creation?.status === 'confirmed') this.creation = null;
    const socket = this.socket;
    this.socket = null;
    this.mods = null;
    this.modsPending = false;
    this.inventory.reset();
    if (socket) socket.close();
  }

  disconnect() {
    this.release();
    this.publish('Disconnected');
  }

  fail(message) {
    this.release();
    this.publish('Error', message);
  }

  connect() {
    this.release();
    this.publish('Connecting');
    let socket;
    try {
      // the GC's own page connects back to the port it came from ("inventory_editor_port"), others to 13001
      const page = globalThis.location;
      const local = page?.protocol === 'http:' && ['127.0.0.1', 'localhost'].includes(page.hostname) && page.port;
      socket = new this.WebSocketClass(`ws://127.0.0.1:${local ? page.port : 13001}`);
    } catch (error) {
      this.fail(error.message);
      return;
    }
    this.socket = socket;
    socket.binaryType = 'arraybuffer';
    this.timer = this.setTimer(() => {
      if (this.socket === socket) this.fail('Timed out waiting for inventory. Check that the game is running and the editor server is enabled.');
    }, 10000);
    socket.onopen = () => {
      if (this.socket === socket) this.publish('Waiting for inventory');
    };
    socket.onmessage = event => {
      if (this.socket !== socket) return;
      try {
        const commands = decodeMessage(event.data);
        if (commands[0]?.type === Command.modList) {
          const { modsOn, loaded, message, mods, general } = commands[0];
          this.mods = { modsOn, loaded, message, mods, general };
          if (this.installing && message.startsWith("Can't install")) this.installing.error = message;
          this.modsPending = false;
          this.onChange(this);
          return;
        }
        const wasReady = this.inventory.ready;
        this.inventory.apply(commands);
        this.trackCreation(commands);
        this.trackDeletion(commands);
        if (this.inventory.ready) {
          this.clearTimer(this.timer);
          this.timer = null;
          this.publish('Connected');
          if (!wasReady) this.requestMods();
        }
      } catch (error) {
        this.fail(error.message);
      }
    };
    socket.onerror = () => {
      if (this.socket === socket) this.fail('Connection failed. Check the game, inventory_editor_origin, and the browser console.');
    };
    socket.onclose = event => {
      if (this.socket !== socket) return;
      this.release();
      this.publish('Disconnected', `GC connection closed (code ${event.code}).`);
    };
  }
}
