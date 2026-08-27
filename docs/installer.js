(() => {
  const select = document.getElementById("ver");
  const button = document.querySelector("esp-web-install-button");
  if (!select || !button) return;

  fetch("versions/index.json")
    .then((response) => (response.ok ? response.json() : []))
    .then((versions) => {
      if (!versions.length) return;
      select.innerHTML = "";
      versions.forEach((version, index) => {
        const option = document.createElement("option");
        option.value = version;
        option.textContent = index === 0 ? `${version} · latest` : version;
        select.appendChild(option);
      });
    })
    .catch(() => {});

  select.addEventListener("change", () => {
    const latest = select.selectedIndex === 0;
    const manifest = latest || !select.value ? "./manifest.json" : `./versions/${select.value}/manifest.json`;
    button.setAttribute("manifest", manifest);
  });
})();
