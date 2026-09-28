// Serialize the evaluated value using the legacy MCP result contract.
// The expression runs once, in the selected document's global scope.
// Browser objects must be inspected here, before CDP converts them to JSON.
(async expression => {
  const raw = await (0, eval)(expression);
  const copied = new WeakMap();
  let visited = 0;
  const transfer = (item, depth = 0) => {
    if (++visited > 100000 || depth > 64)
      throw Error('Legacy evaluation value exceeds traversal limits');
    if (item === undefined || typeof item === 'function' || typeof item === 'symbol')
      return undefined;
    if (item === null || typeof item !== 'object') return item;
    if (item instanceof Window) return 'ref: <Window>';
    if (item instanceof Document) return 'ref: <Document>';
    if (item instanceof Node) return 'ref: <Node>';
    // The old transport reconstructed Date(null) for an invalid date.
    if (item instanceof Date) return new Date(item.toJSON()).toJSON();
    if (item instanceof URL) return item.toJSON();
    if (item instanceof RegExp) return {};
    if (item instanceof Error) return {name: item.name};
    if (copied.has(item)) return copied.get(item);
    const value = Array.isArray(item) ? [] : Object.create(null);
    copied.set(item, value);
    if (Array.isArray(item)) {
      // Functions in transport arrays became null; object fields became undefined.
      for (let i = 0; i < item.length; ++i) {
        const member = item[i];
        value.push(typeof member === 'function' ? null : transfer(member, depth + 1));
      }
    } else {
      for (const key of Object.keys(item)) {
        let member;
        try { member = item[key]; } catch { continue; }
        value[key] = key === 'toJSON' && typeof member === 'function'
          ? {} : transfer(member, depth + 1);
      }
      if (!Object.keys(value).length) {
        let replacement;
        try {
          if (typeof item.toJSON === 'function') replacement = {value: item.toJSON()};
        } catch {}
        if (replacement) return transfer(replacement.value, depth + 1);
      }
    }
    return value;
  };
  const seen = new WeakSet();
  return JSON.stringify(transfer(raw), (_, item) => {
    if (item === undefined) return '__undefined__';
    if (typeof item === 'bigint') return String(item) + 'n';
    if (item && typeof item === 'object') {
      if (seen.has(item)) return '__circular__';
      seen.add(item);
    }
    return item;
  });
})
