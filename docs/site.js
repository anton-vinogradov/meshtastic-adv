(() => {
  "use strict";

  const image = document.getElementById("hero-demo");
  const toggle = document.getElementById("demo-toggle");
  if (!(image instanceof HTMLImageElement) || !(toggle instanceof HTMLButtonElement)) return;

  const poster = image.dataset.poster;
  const animation = image.dataset.animation;
  const duration = Number.parseInt(image.dataset.durationMs || "", 10);
  if (!poster || !animation || !Number.isFinite(duration) || duration <= 0) return;
  toggle.hidden = false;

  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
  const connection = navigator.connection;
  let playing = false;
  let stopTimer = 0;
  const render = () => {
    const source = playing ? animation : poster;
    if (image.getAttribute("src") !== source) image.src = source;
    toggle.textContent = playing ? "Stop 16-second demo" : "Play 16-second demo";
  };
  const stop = () => {
    window.clearTimeout(stopTimer);
    stopTimer = 0;
    playing = false;
    render();
  };
  const play = () => {
    window.clearTimeout(stopTimer);
    playing = true;
    render();
    stopTimer = window.setTimeout(stop, duration);
  };

  toggle.addEventListener("click", () => {
    if (playing) stop();
    else play();
  });
  reducedMotion.addEventListener?.("change", (event) => {
    if (event.matches) stop();
  });
  connection?.addEventListener?.("change", () => {
    if (connection.saveData) stop();
  });

  if (!reducedMotion.matches && !connection?.saveData) play();
  else render();
})();
