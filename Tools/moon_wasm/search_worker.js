import init, { open_index, search_loaded_index } from './pkg/moon_wasm.js';

let initialized = false;

async function ensureInitialized() {
  if (initialized) return;
  await init(new URL('./pkg/moon_wasm_bg.wasm', import.meta.url));
  initialized = true;
}

self.onmessage = async event => {
  const { id, type } = event.data || {};
  try {
    await ensureInitialized();
    if (type === 'open') {
      globalThis.__moonshotIndexFile = event.data.file;
      open_index();
      self.postMessage({ id, ok: true });
      return;
    }
    if (type === 'search') {
      const json = search_loaded_index(event.data.query || '', event.data.streams || 'AUTB');
      self.postMessage({ id, ok: true, json });
      return;
    }
    throw new Error('Unknown search worker request');
  } catch (error) {
    self.postMessage({ id, ok: false, error: String(error?.message || error) });
  }
};
