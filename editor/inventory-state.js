import { Command, PROTOCOL_VERSION } from './protocol.js';

export class InventoryState {
  constructor() { this.reset(); }

  reset() {
    this.items = new Map();
    this.version = null;
    this.ready = false;
  }

  apply(commands) {
    if (this.version === null) {
      if (commands.length !== 1 || commands[0].type !== Command.protocolInfo) {
        throw new Error('GC did not send protocol information. Update the GC.');
      }
      if (commands[0].version !== PROTOCOL_VERSION) {
        throw new Error(`Incompatible protocol: GC ${commands[0].version}, editor ${PROTOCOL_VERSION}.`);
      }
      this.version = commands[0].version;
      return;
    }
    const isSnapshot = commands[0]?.type === Command.snapshot;
    if (!this.ready && !isSnapshot) throw new Error('Expected an initial inventory snapshot.');
    // Validate before mutating the map, including batches passed outside the codec.
    for (let i = 0; i < commands.length; i++) {
      const type = commands[i].type;
      if (type === Command.snapshot && i === 0) continue;
      if (isSnapshot ? type !== Command.create : ![Command.create, Command.modify, Command.destroy, Command.setId].includes(type)) {
        throw new Error('Unexpected inventory command for this connection state.');
      }
    }
    const items = isSnapshot ? new Map() : this.items;
    for (const command of commands) {
      if (command.type === Command.create || command.type === Command.modify) items.set(command.item.highId, command.item);
      if (command.type === Command.destroy) items.delete(command.highId);
      // Assigned IDs belong to connection-level request tracking, not this map.
    }
    this.items = items;
    if (isSnapshot) this.ready = true;
  }
}
