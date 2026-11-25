// Bounded ASCII subset for the optional managed gateway profile. No general XML processor, I/O, entities,
// base URLs, mixed content, DTD, processing instructions, or externally resolved namespaces.
function invalid(): never { throw new Error('Unsupported IGD XML'); }
export class IgdElement {
  name: string = '';
  uri: string = '';
  qname: string = '';
  ns: Map<string, string> = new Map();
  children: IgdElement[] = [];
  text: string = '';
  all(uri: string, name: string): IgdElement[] { return this.children.filter(c => c.uri === uri && c.name === name); }
  optional(uri: string, name: string): IgdElement | undefined {
    const m = this.all(uri, name); if (m.length > 1) invalid(); return m[0];
  }
  one(uri: string, name: string): IgdElement { return this.optional(uri, name) ?? invalid(); }
  value(): string { if (this.children.length || !this.text.trim()) invalid(); return this.text.trim(); }
}
export function igdXml(raw: string): IgdElement {
  if (raw.length > 65536 || /[^\x09\x0a\x0d\x20-\x7e]|&|<!/.test(raw)) invalid();
  const doc = raw.replace(/^<\?xml version=(?:"1\.0"|'1\.0')(?: encoding=(?:"[Uu][Tt][Ff]-8"|'[Uu][Tt][Ff]-8'))?\s*\?>/, '');
  const root = new IgdElement(), stack = [root];
  let i = 0, count = 0;
  while (i < doc.length) {
    const parent = stack[stack.length - 1];
    if (doc[i] !== '<') {
      let end = doc.indexOf('<', i); if (end < 0) end = doc.length;
      const text = doc.substring(i, end); if (text.includes('>')) invalid(); parent.text += text; i = end; continue;
    }
    const end = doc.indexOf('>', i); if (end < 0 || end - i > 2048) invalid();
    let tag = doc.substring(i + 1, end); i = end + 1;
    if (tag.startsWith('/')) {
      if (stack.length < 2 || tag.slice(1).trimEnd() !== parent.qname || (parent.children.length && parent.text.trim())) invalid();
      stack.pop(); continue;
    }
    if (++count > 2048 || stack.length > 24) invalid();
    const single = tag.endsWith('/'); if (single) tag = tag.slice(0, -1);
    const q = /^([A-Za-z_][\w.-]*(?::[A-Za-z_][\w.-]*)?)/.exec(tag); if (!q) invalid();
    const node = new IgdElement(); node.qname = q[1]; node.ns = new Map(parent.ns);
    tag = tag.slice(q[1].length);
    const seen = new Set<string>();
    let encoding = '';
    while (tag.trimEnd().length) {
      const a = /^\s+([A-Za-z_][\w.-]*(?::[A-Za-z_][\w.-]*)?)\s*=\s*(?:"([^"<>]*)"|'([^'<>]*)')/.exec(tag);
      if (!a || seen.has(a[1]) || seen.size >= 16) invalid();
      seen.add(a[1]); tag = tag.slice(a[0].length);
      const key = a[1], value = a[2] ?? a[3];
      if (key === 'xmlns' || key.startsWith('xmlns:')) {
        const prefix = key === 'xmlns' ? '' : key.slice(6);
        if (prefix === 'xml' || prefix === 'xmlns' || (prefix && !value) || value === 'http://www.w3.org/2001/XInclude' ||
          value === 'http://www.w3.org/XML/1998/namespace' || value === 'http://www.w3.org/2000/xmlns/') invalid();
        node.ns.set(prefix, value);
      } else if (key.endsWith(':encodingStyle') && value === 'http://schemas.xmlsoap.org/soap/encoding/' && !encoding) {
        encoding = key.split(':')[0];
      } else if (!(key === 'configId' && /^\d{1,10}$/.test(value))) invalid();
    }
    if (encoding && node.ns.get(encoding) !== 'http://schemas.xmlsoap.org/soap/envelope/') invalid();
    const parts = node.qname.split(':'); node.name = parts[parts.length - 1];
    node.uri = node.ns.get(parts.length === 2 ? parts[0] : '') ?? '';
    if (parts.length === 2 && !node.uri) invalid();
    parent.children.push(node); if (!single) stack.push(node);
  }
  if (stack.length !== 1 || root.children.length !== 1 || root.text.trim()) invalid();
  return root.children[0];
}
