// make_test_docx.mjs - emit a minimal (STORE-method) .docx zip as base64 on
// stdout, for the attachments test's OOXML extraction assertions. A real
// docx is a zip of parts; hand-build it so the test needs no office tooling.
//
// Usage: harness/tests/make_test_docx.mjs  (no args)
import { Buffer } from "node:buffer";

const crc32Table = (() => {
  const table = new Int32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 3988292384 ^ (c >>> 1) : c >>> 1;
    table[i] = c;
  }
  return table;
})();

function crc32(buf) {
  let c = -1;
  for (const b of buf) c = crc32Table[(c ^ b) & 255] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

function localHeader(name, data) {
  const nameBuf = Buffer.from(name);
  const dataBuf = Buffer.from(data);
  const crc = crc32(dataBuf);
  const h = Buffer.alloc(30);
  h.writeUInt32LE(0x04034b50, 0); // local file header signature
  h.writeUInt16LE(20, 4);         // version
  h.writeUInt16LE(0, 6);          // flags
  h.writeUInt16LE(0, 8);          // compression: store
  h.writeUInt16LE(0, 10);         // mtime
  h.writeUInt16LE(0, 12);         // mdate
  h.writeUInt32LE(crc, 14);
  h.writeUInt32LE(dataBuf.length, 18);
  h.writeUInt32LE(dataBuf.length, 22);
  h.writeUInt16LE(nameBuf.length, 26);
  h.writeUInt16LE(0, 28);
  return { header: Buffer.concat([h, nameBuf]), data: dataBuf, crc };
}

function centralHeader(name, data, crc, offset) {
  const nameBuf = Buffer.from(name);
  const h = Buffer.alloc(46);
  h.writeUInt32LE(0x02014b50, 0); // central directory signature
  h.writeUInt16LE(20, 4);
  h.writeUInt16LE(20, 6);
  h.writeUInt16LE(0, 8);
  h.writeUInt16LE(0, 10);
  h.writeUInt16LE(0, 12);
  h.writeUInt32LE(crc, 16);
  h.writeUInt32LE(data.length, 20);
  h.writeUInt32LE(data.length, 24);
  h.writeUInt16LE(nameBuf.length, 28);
  h.writeUInt16LE(0, 30);
  h.writeUInt16LE(0, 32);
  h.writeUInt16LE(0, 34);
  h.writeUInt16LE(0, 36);
  h.writeUInt32LE(0, 40);
  h.writeUInt32LE(offset, 42);
  return Buffer.concat([h, nameBuf]);
}

const text =
  '<?xml version="1.0"?>' +
  '<w:document xmlns:w="w"><w:body>' +
  '<w:p><w:r><w:t>Hello from the OOXML extraction test.</w:t></w:r></w:p>' +
  '<w:p><w:r><w:t>Second paragraph.</w:t></w:r></w:p>' +
  "</w:body></w:document>";

const entries = [
  { name: "word/document.xml", data: text },
  // xlsx builder shares the container logic; a fake spreadsheet part proves
  // the xlsx branch too (shared strings table).
  {
    name: "xl/sharedStrings.xml",
    data:
      '<?xml version="1.0"?>' +
      '<sst xmlns="x"><si><t>Sheet heading from the shared strings test.</t></si>' +
      "<si><t>Cell value one</t></si></sst>",
  },
];

let offset = 0;
const chunks = [];
const centrals = [];
for (const entry of entries) {
  const built = localHeader(entry.name, entry.data);
  chunks.push(built.header, built.data);
  centrals.push(centralHeader(entry.name, entry.data, built.crc, offset));
  offset += built.header.length + built.data.length;
}

const end = Buffer.alloc(22);
end.writeUInt32LE(0x06054b50, 0);
end.writeUInt16LE(0, 4);
end.writeUInt16LE(0, 6);
end.writeUInt16LE(entries.length, 8);
end.writeUInt16LE(entries.length, 10);
let centralSize = 0;
for (const central of centrals) centralSize += central.length;
end.writeUInt32LE(centralSize, 12);
end.writeUInt32LE(offset, 16);
end.writeUInt16LE(0, 20);

// One zip carries BOTH parts (the docx test asserts the document text; the
// xlsx test asserts the shared-strings text - the daemon picks by extension,
// so the same container proves both readers).
console.log(Buffer.concat([...chunks, ...centrals, end]).toString("base64"));
