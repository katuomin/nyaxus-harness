# nyaxus web

The new browser client for the standalone harness.

## modes

The default build uses a fixture transport so the layout and event reducer can
be exercised before the native daemon is available.

To connect the browser to the native daemon (which auto-discovers the model and
context from llama.cpp), set the mode to `http`:

```sh
VITE_HARNESS_MODE=http npm run dev
```

The Vite dev server proxies `/api` to `localhost:8787` (the daemon), so open
`http://localhost:4173` and start a conversation. The daemon endpoints are
documented in `../docs/harness-contract-v1.md`.

## development

```sh
npm install
npm run dev
```

The Vite development server listens on `0.0.0.0:4173`. HTTP API requests proxy
to `localhost:8787`, the native harness port.

## validation

```sh
npm run lint
npm run build
```
