import { translations } from "./i18n.js";
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const language = navigator.language.toLowerCase().startsWith("pt")
  ? "pt"
  : "en";
const text = translations[language];
const $ = (id) => document.getElementById(id);
let files = [];
let busy = false;
const rows = new Map();
document.documentElement.lang = language === "pt" ? "pt-BR" : "en";
for (const element of document.querySelectorAll("[data-text]"))
  element.textContent = text[element.dataset.text];
const size = (bytes) =>
  `${(bytes / 1024 ** 3).toLocaleString(undefined, { maximumFractionDigits: 2 })} GiB`;
function show(screen, n) {
  for (const section of document.querySelectorAll("section"))
    section.hidden = section.id !== screen;
  $("step").textContent = text.step.replace("{n}", n);
  $("status").textContent = "";
  $("warning").textContent = "";
  document.querySelector(`#${screen} h1`).setAttribute("tabindex", "-1");
  document.querySelector(`#${screen} h1`).focus();
}
function failure(error) {
  const key = String(error);
  $("status").textContent =
    text[key] ||
    (key === "portalHttp:401" || key === "portalHttp:403"
      ? text.authFailed
      : text.genericError);
}
async function action(button, work) {
  if (busy) return;
  busy = true;
  button.disabled = true;
  $("status").textContent = text.working;
  try {
    await work();
  } catch (error) {
    failure(error);
  } finally {
    busy = false;
    button.disabled = false;
  }
}
async function chooseHost(host) {
  if (
    !/^(10\.\d+\.\d+\.\d+|192\.168\.\d+\.\d+|172\.(1[6-9]|2\d|3[01])\.\d+\.\d+)$/.test(
      host,
    ) ||
    host.split(".").some((part) => Number(part) > 255)
  )
    throw "privateIpRequired";
  $("host").value = host;
  $("user").value = "";
  $("password").value = "";
  $("remember").checked = false;
  $("selected-host").textContent = host;
  show("login", 2);
  try {
    const saved = await invoke("remembered", { host });
    if (saved) {
      $("user").value = saved.user;
      $("password").value = saved.password;
      $("remember").checked = true;
    }
  } catch (error) {
    failure(error);
  }
}
$("scan").onclick = () =>
  action($("scan"), async () => {
    $("status").textContent = text.scanning;
    $("hosts").replaceChildren();
    const hosts = await invoke("discover");
    $("status").textContent = hosts.length ? "" : text.noneFound;
    for (const host of hosts) {
      const button = document.createElement("button");
      button.className = "secondary mono";
      button.textContent = host;
      button.onclick = () => action(button, () => chooseHost(host));
      $("hosts").append(button);
    }
  });
$("address-form").onsubmit = (event) => {
  event.preventDefault();
  action(event.submitter, () => chooseHost($("host").value.trim()));
};
$("back").onclick = () => {
  if (!busy) show("find", 1);
};
$("login-form").onsubmit = (event) => {
  event.preventDefault();
  action(event.submitter, async () => {
    await invoke("connect", {
      host: $("host").value,
      credentials: { user: $("user").value, password: $("password").value },
      remember: $("remember").checked,
    });
    $("password").value = "";
    show("pick", 3);
  });
};
function renderFiles() {
  $("file-list").replaceChildren();
  for (const file of files) {
    const row = document.createElement("li");
    row.textContent = file.name;
    const detail = document.createElement("small");
    detail.textContent = size(file.size);
    row.append(detail);
    $("file-list").append(row);
  }
  $("send").disabled = !files.length;
  $("clear").hidden = !files.length;
}
for (const [id, folder] of [
  ["files", false],
  ["folder", true],
]) {
  $(id).onclick = () =>
    action($(id), async () => {
      const selected = await invoke("pick_files", { folder });
      if (selected.length) files = selected;
      renderFiles();
      $("status").textContent = "";
    });
}
$("clear").onclick = () =>
  action($("clear"), async () => {
    await invoke("clear_files");
    files = [];
    renderFiles();
    $("status").textContent = "";
  });
await listen("upload-progress", ({ payload }) => {
  let row = rows.get(payload.file);
  if (!row) {
    row = document.createElement("li");
    const name = document.createElement("span");
    name.textContent = text.gameDestination
      .replace("{file}", payload.file)
      .replace("{folder}", payload.directory);
    const detail = document.createElement("small");
    const progress = document.createElement("progress");
    progress.setAttribute("aria-label", payload.file);
    row.append(name, detail, progress);
    rows.set(payload.file, row);
    $("progress-list").append(row);
  }
  row.querySelector("progress").max = payload.total;
  row.querySelector("progress").value = payload.sent;
  row.querySelector("small").textContent = payload.completed
    ? `${text.verified} · ${size(payload.total)}`
    : `${Math.floor((payload.sent / payload.total) * 100)}% · ${size(payload.sent)} / ${size(payload.total)}`;
  $("status").textContent = "";
});
await listen("upload-warning", ({ payload }) => {
  $("warning").textContent = text[payload] || text.genericError;
});
$("send").onclick = () =>
  action($("send"), async () => {
    show("transfer", 4);
    rows.clear();
    $("progress-list").replaceChildren();
    $("transfer-title").textContent = text.sending;
    $("transfer-detail").textContent = text.keepOpen;
    $("status").textContent = text.preparing;
    $("cancel").hidden = false;
    $("cancel").disabled = false;
    $("again").hidden = true;
    try {
      const result = await invoke("send");
      $("transfer-title").textContent = text.done;
      $("status").textContent = text.summary
        .replace("{files}", result.files)
        .replace("{size}", size(result.bytes));
      $("warning").textContent = result.warnings
        .map((key) => text[key])
        .join(" ");
    } catch (error) {
      $("transfer-title").textContent = text.failed;
      failure(error);
    } finally {
      $("cancel").hidden = true;
      $("again").hidden = false;
    }
  });
$("cancel").onclick = async () => {
  $("cancel").disabled = true;
  try {
    await invoke("cancel");
  } catch (error) {
    failure(error);
  }
};
$("again").onclick = () => {
  if (!busy) show("pick", 3);
};
$("folder").hidden = !(await invoke("can_pick_folder"));
show("find", 1);
$("scan").click();
