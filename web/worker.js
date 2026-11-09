let engine;
const snapshot = () => JSON.parse(engine.UTF8ToString(engine._pig_snapshot()));
const send = (id, type, extra = {}) => postMessage({id, type, ...extra});

onmessage = async ({data}) => {
  const {id, type} = data;
  try {
    if (type === 'init') {
      const threaded = data.threaded && self.crossOriginIsolated;
      const script = new URL(threaded ? 'pig-threaded.js' : 'pig-single.js', self.location.href).href;
      importScripts(script);
      engine = await createPig({
        mainScriptUrlOrBlob: script,
        locateFile: path => new URL(path, self.location.href).href,
        onAbort: reason => send(id, 'error', {message: `Engine stopped: ${reason}`}),
      });
      const workers = threaded ? Math.min(4, Math.max(1, data.workers)) : 1;
      if (!engine._pig_init(workers)) throw Error('The model could not be loaded.');
      send(id, 'ready', {workers});
    } else if (type === 'new' || type === 'restore') {
      if (!Number.isInteger(data.seed) || data.seed < 0 || data.seed > 0xffffffff || ![0, 1].includes(data.human)) throw Error('Invalid game settings.');
      engine._pig_new(data.seed, data.human);
      if (type === 'restore') {
        if (!Array.isArray(data.history) || data.history.length > 10000) throw Error('Invalid saved game.');
        for (const action of data.history) {
          if (!Number.isInteger(action) || action < 0 || action >= 249 || !engine._pig_replay(action)) throw Error('Saved game contains an invalid move.');
        }
      }
      send(id, 'state', {state: snapshot(), restored: type === 'restore'});
    } else if (type === 'act') {
      if (!Number.isInteger(data.action) || !engine._pig_act(data.action)) throw Error('That move is not legal now.');
      send(id, 'state', {state: snapshot()});
    } else if (type === 'bot') {
      const start = performance.now();
      if (engine._pig_bot() < 0) throw Error('The bot could not make a legal move.');
      send(id, 'state', {state: snapshot(), elapsed: performance.now() - start});
    }
  } catch (error) {
    send(id, 'error', {message: error.message || String(error)});
  }
};
