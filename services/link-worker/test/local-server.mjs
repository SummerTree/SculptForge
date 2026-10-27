// Runs the relay on http://127.0.0.1:<port>/relay/ with in-memory Durable Objects, for trying the desktop and the
// face page against it without Cloudflare:  node test/local-server.mjs [port]
//   SCULPTFORGE_LINK_RELAY=http://127.0.0.1:8787/relay/ ./build/sculptforge
import http from "node:http";
import worker from "../src/index.js";
import { fakeEnv } from "./fake-env.mjs";

const port = Number(process.argv[2] ?? 8787);
const env = fakeEnv(process.env.ALLOWED_ORIGINS ?? "https://sculptforge.app,http://localhost:8000");
setInterval(() => env.fireAlarms(), 10_000).unref();

http.createServer(async (req, res) => {
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const body = chunks.length && req.method !== "GET" ? Buffer.concat(chunks) : undefined;
    const request = new Request(`http://127.0.0.1:${port}${req.url}`, { method: req.method, headers: req.headers, body });
    const response = await worker.fetch(request, env);
    res.writeHead(response.status, Object.fromEntries(response.headers));
    res.end(Buffer.from(await response.arrayBuffer()));
}).listen(port, "127.0.0.1", () => console.log(`relay on http://127.0.0.1:${port}/relay/`));
