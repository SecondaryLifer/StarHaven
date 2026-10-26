// pack.js: reads tools/pack.py's packs ("SHPK", index, files; gzipped).

/** fetch and unpack: Map(path -> Uint8Array); onProgress(bytes, total) while downloading */
export async function loadPack(url, onProgress = () => {}) {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`couldn't download ${url} (${res.status})`);
    const total = Number(res.headers.get("content-length")) || 0;
    let got = 0;
    const counted = res.body.pipeThrough(new TransformStream({
        transform(chunk, ctl) { got += chunk.length; onProgress(got, total); ctl.enqueue(chunk); },
    }));
    const raw = new Uint8Array(await new Response(counted.pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
    return unpack(raw);
}

export function unpack(raw) {
    if (String.fromCharCode(...raw.subarray(0, 4)) !== "SHPK") throw new Error("not a converter pack");
    const n = new DataView(raw.buffer, raw.byteOffset).getUint32(4, true);
    const index = JSON.parse(new TextDecoder().decode(raw.subarray(8, 8 + n)));
    const files = new Map();
    let o = 8 + n;
    for (const [path, size] of index) {
        files.set(path, raw.subarray(o, o + size));
        o += size;
    }
    return files;
}
