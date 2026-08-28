(() => {
  "use strict";

  const image = document.getElementById("hero-demo");
  const toggle = document.getElementById("demo-toggle");
  if (!(image instanceof HTMLImageElement) || !(toggle instanceof HTMLButtonElement)) return;

  const poster = image.dataset.poster;
  const animation = image.dataset.animation;
  if (!poster || !animation) return;
  toggle.hidden = false;

  let playing = false;
  const render = () => {
    image.src = playing ? animation : poster;
    toggle.textContent = playing ? "Stop animation" : "Play 16-second demo";
  };

  toggle.addEventListener("click", () => {
    playing = !playing;
    render();
  });
  render();
})();
