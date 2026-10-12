"use strict";

const elements = {
  bridgeDot: document.querySelector("#bridge-dot"),
  bridgeStatus: document.querySelector("#bridge-status"),
  deviceDot: document.querySelector("#device-dot"),
  deviceStatus: document.querySelector("#device-status"),
  previewSurface: document.querySelector("#preview-surface"),
  previewVideo: document.querySelector("#preview-video"),
  emptyPreview: document.querySelector("#empty-preview"),
  previewName: document.querySelector("#preview-name"),
  dropZone: document.querySelector("#drop-zone"),
  fileInput: document.querySelector("#file-input"),
  dropTitle: document.querySelector("#drop-title"),
  dropSubtitle: document.querySelector("#drop-subtitle"),
  fpsSelect: document.querySelector("#fps-select"),
  uploadButton: document.querySelector("#upload-button"),
  jobStatus: document.querySelector("#job-status"),
  jobTitle: document.querySelector("#job-title"),
  jobDetail: document.querySelector("#job-detail"),
  jobProgress: document.querySelector("#job-progress"),
  librarySummary: document.querySelector("#library-summary"),
  libraryMessage: document.querySelector("#library-message"),
  refreshButton: document.querySelector("#refresh-button"),
  wallpaperGrid: document.querySelector("#wallpaper-grid"),
  emptyLibrary: document.querySelector("#empty-library"),
  deleteDialog: document.querySelector("#delete-dialog"),
  deleteDialogTitle: document.querySelector("#delete-dialog-title"),
  deleteDialogDescription: document.querySelector("#delete-dialog-description"),
  deleteDialogImpact: document.querySelector("#delete-dialog-impact"),
  deleteDialogError: document.querySelector("#delete-dialog-error"),
  deleteCancel: document.querySelector("#delete-cancel"),
  deleteConfirm: document.querySelector("#delete-confirm"),
};

let selectedFile = null;
let previewObjectUrl = null;
let previewedWallpaperId = null;
let busy = false;
let deleteTarget = null;
let deleteTrigger = null;
let deleteInProgress = false;
let restoreDeleteFocus = true;

function fitMode() {
  return document.querySelector('input[name="fit"]:checked').value;
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) return "—";
  if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

function formatDuration(seconds) {
  if (!Number.isFinite(seconds)) return "—";
  const rounded = Math.max(0, Math.round(seconds));
  const minutes = Math.floor(rounded / 60);
  const remainder = rounded % 60;
  return `${minutes}:${String(remainder).padStart(2, "0")}`;
}

function showJob(title, detail, value, indeterminate = false) {
  elements.jobStatus.hidden = false;
  elements.jobTitle.textContent = title;
  elements.jobDetail.textContent = detail;
  if (indeterminate) {
    elements.jobProgress.removeAttribute("value");
  } else {
    elements.jobProgress.value = Math.max(0, Math.min(100, value));
  }
}

function setBusy(value) {
  busy = value;
  elements.uploadButton.disabled = value || !selectedFile;
  elements.fileInput.disabled = value;
  elements.fpsSelect.disabled = value;
  document.querySelectorAll('input[name="fit"]').forEach((input) => {
    input.disabled = value;
  });
  elements.refreshButton.disabled = value;
  elements.wallpaperGrid.setAttribute("aria-busy", String(value));
  elements.wallpaperGrid.querySelectorAll("button").forEach((button) => {
    button.disabled = value || button.dataset.alwaysDisabled === "1";
  });
}

function setPreview(source, name, fit, objectUrl = false, wallpaperId = null) {
  if (previewObjectUrl && !objectUrl) {
    URL.revokeObjectURL(previewObjectUrl);
    previewObjectUrl = null;
  }
  elements.previewSurface.classList.toggle("contain", fit === "contain");
  elements.previewVideo.src = source;
  elements.previewVideo.classList.add("visible");
  elements.emptyPreview.hidden = true;
  elements.previewName.textContent = name || "LIVE PREVIEW";
  previewedWallpaperId = wallpaperId;
  const playPromise = elements.previewVideo.play();
  if (playPromise) playPromise.catch(() => {});
}

function clearPreview() {
  if (previewObjectUrl) {
    URL.revokeObjectURL(previewObjectUrl);
    previewObjectUrl = null;
  }
  elements.previewVideo.pause();
  elements.previewVideo.removeAttribute("src");
  elements.previewVideo.load();
  elements.previewVideo.classList.remove("visible");
  elements.previewSurface.classList.remove("contain");
  elements.emptyPreview.hidden = false;
  elements.previewName.textContent = "LIVE PREVIEW";
  previewedWallpaperId = null;
}

function selectFile(file) {
  if (!file) return;
  const looksLikeVideo = file.type.startsWith("video/") || /\.(mp4|mov|m4v|webm|mkv)$/i.test(file.name);
  if (!looksLikeVideo) {
    showJob("无法选择该文件", "请选择常见的视频文件", 0);
    return;
  }
  if (file.size <= 0 || file.size > 800 * 1024 * 1024) {
    showJob("文件大小不符合限制", "视频必须大于 0 且不超过 800 MB", 0);
    return;
  }
  selectedFile = file;
  if (previewObjectUrl) URL.revokeObjectURL(previewObjectUrl);
  previewObjectUrl = URL.createObjectURL(file);
  setPreview(previewObjectUrl, file.name, fitMode(), true, null);
  elements.dropTitle.textContent = file.name;
  elements.dropSubtitle.textContent = `${formatBytes(file.size)} · 点击可重新选择`;
  elements.uploadButton.disabled = busy;
  elements.jobStatus.hidden = true;
}

async function parseResponse(response) {
  let payload = {};
  try {
    payload = await response.json();
  } catch (_error) {
    payload = {};
  }
  if (!response.ok) {
    const error = new Error(payload.error || `请求失败 (${response.status})`);
    error.status = response.status;
    error.code = payload.code || "";
    throw error;
  }
  return payload;
}

function showLibraryMessage(message, isError = false) {
  elements.libraryMessage.hidden = false;
  elements.libraryMessage.classList.toggle("error", isError);
  elements.libraryMessage.textContent = message;
}

function uploadSelectedFile() {
  if (!selectedFile || busy) return;
  setBusy(true);
  showJob("正在上传到本机 Bridge", `0% · ${formatBytes(selectedFile.size)}`, 0);
  const query = new URLSearchParams({
    name: selectedFile.name,
    fit: fitMode(),
    fps: elements.fpsSelect.value,
    activate: "1",
  });
  const request = new XMLHttpRequest();
  request.open("POST", `/api/wallpapers?${query.toString()}`);
  request.setRequestHeader("Content-Type", "application/octet-stream");
  request.timeout = 20 * 60 * 1000;
  request.upload.addEventListener("progress", (event) => {
    if (!event.lengthComputable) {
      showJob("正在上传到本机 Bridge", formatBytes(event.loaded), 0, true);
      return;
    }
    const percent = Math.round((event.loaded / event.total) * 100);
    showJob("正在上传到本机 Bridge", `${percent}% · ${formatBytes(event.loaded)}`, percent);
    if (percent >= 100) {
      showJob("正在为 ESP32 转码", "根据视频长度，可能需要几十秒到几分钟", 100, true);
    }
  });
  request.addEventListener("load", async () => {
    let payload = {};
    try {
      payload = JSON.parse(request.responseText || "{}");
    } catch (_error) {
      payload = {};
    }
    if (request.status < 200 || request.status >= 300) {
      showJob("导入失败", payload.error || `Bridge 返回 ${request.status}`, 0);
      setBusy(false);
      return;
    }
    const imported = payload.wallpaper && typeof payload.wallpaper === "object"
      ? payload.wallpaper
      : null;
    selectedFile = null;
    elements.fileInput.value = "";
    elements.dropTitle.textContent = "拖放视频到这里";
    elements.dropSubtitle.textContent = "或点击选择电脑中的文件";
    if (imported) previewRecord(imported);
    showJob("视频已导入，正在应用", "请查看设备状态；SD 壁纸需保存、校验后开始播放", 100);
    setBusy(false);
    await Promise.all([loadLibrary(), loadStatus()]);
  });
  request.addEventListener("error", () => {
    showJob("上传失败", "无法连接本机 Bridge", 0);
    setBusy(false);
  });
  request.addEventListener("timeout", () => {
    showJob("处理超时", "视频可能已导入，请刷新壁纸库确认", 0);
    setBusy(false);
  });
  request.send(selectedFile);
}

function makeElement(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}

function previewRecord(record) {
  setPreview(record.media_url, record.name, record.fit, false, record.id);
}

async function activateRecord(record, button) {
  if (busy) return;
  button.disabled = true;
  button.textContent = "正在应用…";
  try {
    const response = await fetch("/api/active", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id: record.id }),
    });
    await parseResponse(response);
    showJob("已提交壁纸切换", `${record.name} · 等待设备保存并确认播放`, 100);
    previewRecord(record);
    await Promise.all([loadLibrary(), loadStatus()]);
  } catch (error) {
    showJob("应用失败", error.message, 0);
    button.disabled = false;
    button.textContent = "应用到 ESP32";
  }
}

function openDeleteDialog(record, button, index) {
  if (busy || elements.deleteDialog.open) return;
  deleteTarget = { record, index };
  deleteTrigger = button;
  restoreDeleteFocus = true;
  elements.deleteDialogTitle.textContent = `删除“${record.name}”？`;
  elements.deleteDialogDescription.textContent = record.active
    ? "这张壁纸正在使用。删除后，已连接的 ESP32 会清除对应壁纸并回到内置动态背景；离线设备会在下次连接时同步清除。"
    : "将从这台 Mac 的壁纸库中永久删除源视频副本、预览图和已转码帧。";
  elements.deleteDialogImpact.textContent =
    "你最初选择的文件不会受影响。此操作无法撤销。";
  elements.deleteDialogError.hidden = true;
  elements.deleteDialogError.textContent = "";
  elements.deleteCancel.disabled = false;
  elements.deleteConfirm.disabled = false;
  elements.deleteConfirm.textContent = "永久删除";
  elements.deleteDialog.setAttribute("aria-busy", "false");
  elements.deleteDialog.showModal();
  window.requestAnimationFrame(() => elements.deleteCancel.focus());
}

function closeDeleteDialog(restoreFocus = true) {
  restoreDeleteFocus = restoreFocus;
  if (elements.deleteDialog.open) elements.deleteDialog.close();
}

function focusLibraryAfterDelete(index) {
  const previews = elements.wallpaperGrid.querySelectorAll(".card-preview");
  if (previews.length) {
    previews[Math.min(index, previews.length - 1)].focus();
  } else {
    elements.librarySummary.focus();
  }
}

async function confirmDelete() {
  if (!deleteTarget || deleteInProgress) return;
  const { record, index } = deleteTarget;
  const deletedPreviewWasVisible = previewedWallpaperId === record.id;
  let focusAfterReconcile = false;
  deleteInProgress = true;
  setBusy(true);
  elements.deleteDialog.setAttribute("aria-busy", "true");
  elements.deleteCancel.disabled = true;
  elements.deleteConfirm.disabled = true;
  elements.deleteConfirm.textContent = "正在删除…";
  elements.deleteDialogError.hidden = true;

  try {
    const response = await fetch(`/api/wallpapers/${encodeURIComponent(record.id)}`, {
      method: "DELETE",
    });
    const payload = await parseResponse(response);
    closeDeleteDialog(false);
    const records = await loadLibrary();
    await loadStatus();
    if (deletedPreviewWasVisible) {
      const active = Array.isArray(records)
        ? records.find((item) => item.active)
        : null;
      if (active) previewRecord(active);
      else clearPreview();
    }
    const fallback = payload.deleted_active
      ? " 已删除当前壁纸并发送设备清除请求；离线设备会在下次连接时同步清除。"
      : "";
    const cleanup = payload.cleanup_pending
      ? " 部分已删除文件将在 Bridge 下次启动时继续清理。"
      : "";
    showLibraryMessage(
      `已从本机壁纸库删除“${record.name}”。原始文件未受影响。${fallback}${cleanup}`,
    );
    focusAfterReconcile = true;
    deleteTarget = null;
    deleteTrigger = null;
  } catch (error) {
    if (error.status === 404) {
      closeDeleteDialog(false);
      const records = await loadLibrary();
      await loadStatus();
      if (deletedPreviewWasVisible) {
        const active = Array.isArray(records)
          ? records.find((item) => item.active)
          : null;
        if (active) previewRecord(active);
        else clearPreview();
      }
      showLibraryMessage(`“${record.name}”已不在壁纸库，列表已刷新。`);
      focusAfterReconcile = true;
      deleteTarget = null;
      deleteTrigger = null;
    } else {
      const uncertain = error.code === "outcome_unknown" || !error.status || error.status >= 500;
      elements.deleteDialogError.textContent = uncertain
        ? "无法确认删除结果。请刷新壁纸库后再重试。"
        : error.message;
      elements.deleteDialogError.hidden = false;
      elements.deleteCancel.disabled = false;
      elements.deleteConfirm.disabled = false;
      elements.deleteConfirm.textContent = "重新尝试删除";
      elements.deleteDialog.setAttribute("aria-busy", "false");
    }
  } finally {
    deleteInProgress = false;
    setBusy(false);
    if (focusAfterReconcile) focusLibraryAfterDelete(index);
  }
}

function renderLibrary(records) {
  elements.wallpaperGrid.replaceChildren();
  elements.emptyLibrary.hidden = records.length !== 0;
  elements.librarySummary.textContent = records.length
    ? `${records.length} 个本机视频 · 点击画面可预览`
    : "壁纸库为空";
  records.forEach((record, index) => {
    const card = makeElement("article", `wallpaper-card${record.active ? " active" : ""}`);
    const preview = makeElement("button", "card-preview");
    preview.type = "button";
    preview.setAttribute("aria-label", `预览 ${record.name}`);
    const image = document.createElement("img");
    image.src = record.thumbnail_url;
    image.alt = "";
    preview.append(image);
    if (record.active) preview.append(makeElement("span", "active-badge", "正在使用"));
    preview.addEventListener("click", () => previewRecord(record));

    const body = makeElement("div", "card-body");
    const titleRow = makeElement("div", "card-title-row");
    titleRow.append(makeElement("strong", "", record.name));
    titleRow.append(makeElement("span", "", `${record.fps} FPS`));
    const meta = makeElement("div", "card-meta");
    meta.append(makeElement("span", "", formatDuration(record.duration_seconds)));
    meta.append(makeElement("span", "", record.fit === "cover" ? "铺满" : "完整显示"));
    meta.append(makeElement("span", "", formatBytes(record.frame_bytes)));
    const apply = makeElement(
      "button",
      "apply-button",
      record.active ? "重新应用" : "应用到 ESP32",
    );
    apply.type = "button";
    apply.dataset.alwaysDisabled = "0";
    apply.disabled = Boolean(busy);
    apply.addEventListener("click", () => activateRecord(record, apply));
    const remove = makeElement("button", "delete-button", "删除");
    remove.type = "button";
    remove.disabled = busy;
    remove.setAttribute("aria-label", `删除“${record.name}”`);
    remove.addEventListener("click", () => openDeleteDialog(record, remove, index));
    const actions = makeElement("div", "card-actions");
    actions.append(apply, remove);
    body.append(titleRow, meta, actions);
    card.append(preview, body);
    elements.wallpaperGrid.append(card);
  });
}

async function loadLibrary() {
  try {
    const response = await fetch("/api/wallpapers", { cache: "no-store" });
    const payload = await parseResponse(response);
    const records = Array.isArray(payload.wallpapers) ? payload.wallpapers : [];
    renderLibrary(records);
    const active = records.find((record) => record.active);
    if (active && !selectedFile) previewRecord(active);
    return records;
  } catch (error) {
    elements.librarySummary.textContent = `无法读取壁纸库：${error.message}`;
    return null;
  }
}

async function loadStatus() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    const payload = await parseResponse(response);
    renderOtaStatus(payload);
    elements.bridgeDot.className = "online";
    elements.bridgeStatus.textContent = "Bridge 已就绪";
    const clients = Number(payload.ready_clients || 0);
    if (clients > 0) {
      const state = payload.wallpaper_state;
      if (state === "playing") {
        elements.deviceDot.className = "online";
        const local = payload.playback_mode === "sd_local";
        elements.deviceStatus.textContent = `${clients} 台 ESP32 已连接 · ${local ? "SD 卡本地播放" : "正在播放"}`;
      } else if (state === "uploading") {
        elements.deviceDot.className = "warn";
        const transfer = (payload.sd_transfers || [])[0];
        const percent = transfer && transfer.total_bytes ? Math.floor(100 * transfer.received_bytes / transfer.total_bytes) : 0;
        elements.deviceStatus.textContent = `${clients} 台 ESP32 已连接 · 保存到 SD 卡 ${percent}%`;
      } else if (state === "verifying") {
        elements.deviceDot.className = "warn";
        elements.deviceStatus.textContent = `${clients} 台 ESP32 已连接 · 校验本地壁纸`;
      } else if (state === "cache_error") {
        elements.deviceDot.className = "warn";
        elements.deviceStatus.textContent = `${clients} 台 ESP32 已连接 · SD 保存失败，保留原壁纸；请重试应用`;
      } else if (state === "decode_error") {
        elements.deviceDot.className = "warn";
        elements.deviceStatus.textContent = `${clients} 台 ESP32 已连接 · 视频解码失败`;
      } else if (state === "selected" || state === "pending") {
        elements.deviceDot.className = "warn";
        elements.deviceStatus.textContent = `${clients} 台 ESP32 已连接 · 等待首帧`;
      } else {
        elements.deviceDot.className = "online";
        elements.deviceStatus.textContent = `${clients} 台 ESP32 已连接 · 内置壁纸`;
      }
    } else {
      elements.deviceDot.className = "warn";
      elements.deviceStatus.textContent = "ESP32 尚未连接";
    }
  } catch (_error) {
    otaUi.ready = false;
    otaUi.send.disabled = true;
    otaUi.device.textContent = "设备状态未知，请检查 Bridge";
    elements.bridgeDot.className = "warn";
    elements.bridgeStatus.textContent = "Bridge 状态不可用";
    elements.deviceDot.className = "";
    elements.deviceStatus.textContent = "ESP32 状态未知";
  }
}

// ----- Screen & brightness ------------------------------------------------
const display = {
  form: document.querySelector("#display-form"),
  nightStart: document.querySelector("#night-start"),
  nightEnd: document.querySelector("#night-end"),
  nightLevel: document.querySelector("#night-level"),
  nightWake: document.querySelector("#night-wake-level"),
  dayLevel: document.querySelector("#day-level"),
  idleDim: document.querySelector("#idle-dim"),
  idleLevel: document.querySelector("#idle-level"),
  save: document.querySelector("#display-save"),
  message: document.querySelector("#display-message"),
};
const levelOutputs = [
  [display.nightLevel, "#night-level-value"],
  [display.nightWake, "#night-wake-value"],
  [display.dayLevel, "#day-level-value"],
  [display.idleLevel, "#idle-level-value"],
];

function minutesToTime(minutes) {
  return `${String(Math.floor(minutes / 60)).padStart(2, "0")}:${String(minutes % 60).padStart(2, "0")}`;
}
function timeToMinutes(value) {
  const [hours, minutes] = value.split(":").map(Number);
  return hours * 60 + minutes;
}
function showDisplayMessage(text, isError = false) {
  display.message.textContent = text;
  display.message.classList.toggle("error", isError);
}
function refreshLevelOutputs() {
  for (const [input, selector] of levelOutputs) {
    document.querySelector(selector).textContent = `${input.value}%`;
  }
}
function fillDisplayForm(settings) {
  display.nightStart.value = minutesToTime(settings.night_start_min);
  display.nightEnd.value = minutesToTime(settings.night_end_min);
  display.nightLevel.value = settings.night_level;
  display.nightWake.value = settings.night_wake_level;
  display.dayLevel.value = settings.day_level;
  display.idleLevel.value = settings.idle_level;
  const idle = String(settings.idle_dim_s);
  if (![...display.idleDim.options].some((option) => option.value === idle)) {
    display.idleDim.append(new Option(`${Math.round(settings.idle_dim_s / 60)} 分钟`, idle));
  }
  display.idleDim.value = idle;
  refreshLevelOutputs();
}
async function loadDisplaySettings() {
  try {
    const response = await fetch("/api/display-settings", { cache: "no-store" });
    fillDisplayForm(await parseResponse(response));
  } catch (error) {
    showDisplayMessage(`读取屏幕设置失败：${error.message}`, true);
  }
}
async function saveDisplaySettings(event) {
  event.preventDefault();
  const settings = {
    night_start_min: timeToMinutes(display.nightStart.value),
    night_end_min: timeToMinutes(display.nightEnd.value),
    night_level: Number(display.nightLevel.value),
    night_wake_level: Number(display.nightWake.value),
    day_level: Number(display.dayLevel.value),
    idle_dim_s: Number(display.idleDim.value),
    idle_level: Number(display.idleLevel.value),
  };
  // Keep the same invariants the Bridge and firmware enforce.
  if (settings.night_wake_level < settings.night_level) {
    showDisplayMessage("夜里触摸后的亮度不能低于夜间亮度", true);
    return;
  }
  if (settings.idle_level > settings.day_level) {
    showDisplayMessage("变暗后的亮度不能高于白天亮度", true);
    return;
  }
  display.save.disabled = true;
  showDisplayMessage("正在保存…");
  try {
    const response = await fetch("/api/display-settings", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(settings),
    });
    const result = await parseResponse(response);
    fillDisplayForm(result.settings);
    showDisplayMessage(result.delivered_clients > 0
      ? "已保存并同步到 ESP32"
      : "已保存；ESP32 下次连接时自动同步");
  } catch (error) {
    showDisplayMessage(`保存失败：${error.message}`, true);
  } finally {
    display.save.disabled = false;
  }
}
for (const [input] of levelOutputs) input.addEventListener("input", refreshLevelOutputs);
display.form.addEventListener("submit", saveDisplaySettings);


const otaUi = {
  file: document.querySelector("#ota-file"), send: document.querySelector("#ota-send"),
  message: document.querySelector("#ota-message"), device: document.querySelector("#ota-device"),
  progress: document.querySelector("#ota-progress"), ready: false, active: false, sending: false,
};
function renderOtaStatus(payload) {
  const devices = payload.devices || [];
  otaUi.ready = devices.filter((d) => d.paired && d.ota).length === 1;
  otaUi.device.textContent = devices.length ? devices.map((d) => `ESP32 · ${d.firmware}${d.ota && d.paired ? " · 支持无线升级" : " · 需先安装支持无线升级的固件并配对"}`).join(" / ") : "ESP32 尚未连接";
  const update = payload.ota;
  otaUi.active = update && !["complete", "error", "cancelled", "rolled_back"].includes(update.state);
  otaUi.send.disabled = !otaUi.ready || otaUi.active || otaUi.sending || !otaUi.file.files.length;
  if (!update) {
    if (!otaUi.sending) otaUi.message.textContent = "选择 firmware.bin，发送后在设备上确认安装。";
    otaUi.progress.hidden = true;
    otaUi.progress.value = 0;
    return;
  }
  const labels = {
    offered: "等待你在 ESP32 上长按安装", uploading: `正在传输 ${update.percent}%`,
    verifying: "正在校验固件", rebooting: "设备正在重启", validating: "设备启动自检中，请保持供电",
    complete: "升级完成，设备已通过启动自检", cancelled: "更新已取消",
    error: "更新未完成，请检查连接后重试", rolled_back: "新固件未通过启动检查，已回滚",
    unconfirmed: "尚未收到启动确认，请检查设备状态",
  };
  otaUi.message.textContent = `${update.version} · ${labels[update.state] || "等待设备状态"}`;
  otaUi.progress.hidden = ["offered", "cancelled", "error", "rolled_back"].includes(update.state);
  otaUi.progress.value = update.percent;
}
otaUi.file.addEventListener("change", () => loadStatus());
otaUi.send.addEventListener("click", async () => {
  const file = otaUi.file.files[0];
  if (!file || !otaUi.ready || otaUi.active || otaUi.sending) return;
  if (file.size < 288 || file.size > 0x640000) {
    otaUi.message.textContent = "请选择有效的固件文件，最大 6.25 MiB";
    return;
  }
  otaUi.sending = true; otaUi.send.disabled = true;
  otaUi.message.textContent = "正在检查并发送固件…";
  try {
    await parseResponse(await fetch("/api/ota", {method: "POST", headers: {"Content-Type": "application/octet-stream"}, body: file}));
    await loadStatus();
  } catch (error) {
    otaUi.message.textContent = `无法发起更新：${error.message}`;
  } finally {
    otaUi.sending = false;
    otaUi.send.disabled = !otaUi.ready || otaUi.active || !otaUi.file.files.length;
  }
});

elements.fileInput.addEventListener("change", () => selectFile(elements.fileInput.files[0]));
elements.uploadButton.addEventListener("click", uploadSelectedFile);
elements.refreshButton.addEventListener("click", () => Promise.all([loadLibrary(), loadStatus()]));
elements.deleteCancel.addEventListener("click", () => {
  if (!deleteInProgress) closeDeleteDialog(true);
});
elements.deleteConfirm.addEventListener("click", confirmDelete);
elements.deleteDialog.addEventListener("cancel", (event) => {
  if (deleteInProgress) event.preventDefault();
});
elements.deleteDialog.addEventListener("close", () => {
  if (restoreDeleteFocus && deleteTrigger && deleteTrigger.isConnected) {
    deleteTrigger.focus();
  }
  if (!deleteInProgress) {
    deleteTarget = null;
    deleteTrigger = null;
  }
});
document.querySelectorAll('input[name="fit"]').forEach((input) => {
  input.addEventListener("change", () => {
    elements.previewSurface.classList.toggle("contain", fitMode() === "contain");
  });
});

for (const eventName of ["dragenter", "dragover"]) {
  elements.dropZone.addEventListener(eventName, (event) => {
    event.preventDefault();
    if (!busy) elements.dropZone.classList.add("dragging");
  });
}
for (const eventName of ["dragleave", "drop"]) {
  elements.dropZone.addEventListener(eventName, (event) => {
    event.preventDefault();
    elements.dropZone.classList.remove("dragging");
  });
}
elements.dropZone.addEventListener("drop", (event) => {
  if (!busy) selectFile(event.dataTransfer.files[0]);
});

window.addEventListener("beforeunload", () => {
  if (previewObjectUrl) URL.revokeObjectURL(previewObjectUrl);
});

Promise.all([loadLibrary(), loadStatus(), loadDisplaySettings()]);
window.setInterval(loadStatus, 3000);


// Bounded desktop hub. Render user data only through textContent.
const hub = { busy:false, muted:false, data:{}, volumeDragging:false };
const hubEl = (id) => document.getElementById(`hub-${id}`);
const hubState = { running:"进行中", succeeded:"成功", failed:"失败", unconfirmed:"结果未确认", waiting_input:"等待输入", success:"成功", stale:"长时间未更新", round_done:"本轮结束", round_stopped:"本轮已停止", fresh:"最新", error:"读取失败", not_provided:"暂未提供" };
async function hubRequest(section, payload) {
  const response = await fetch(`/api/hub/${section}`, payload ? {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(payload)} : {});
  const data = await response.json();
  if(!response.ok) throw new Error(data.error || "桌面工具读取失败");
  return data;
}
function hubMessage(value) { hubEl("feedback").textContent = value; }
async function hubPoll(oid) {
  for(let i=0;i<90;i++) {
    await new Promise(resolve => setTimeout(resolve,1000));
    const response = await fetch(`/api/hub/actions?operation_id=${encodeURIComponent(oid)}`);
    const result = await response.json();
    if(result.state !== "running" && result.state !== "not_provided") {
      hubMessage(`${hubState[result.state] || result.state}：${result.reason || ""}`); await hubRefresh(); return result;
    }
  }
  hubMessage("结果未确认：请刷新实际状态。网络切换不会自动重复执行。");
}
async function hubAction(action,target="",value) {
  if(hub.busy) return;
  hub.busy=true; hubMessage("操作进行中…");
  try {
    const payload={operation_id:crypto.randomUUID().replaceAll("-", ""),action};
    if(target) payload.target=target;
    if(value !== undefined) payload.value=value;
    const result=await hubRequest("actions",payload);
    if(result.state==="running")await hubPoll(result.operation_id);else hubMessage(`${hubState[result.state] || result.state}：${result.reason || ""}`);
  } catch(error) { hubMessage(`结果未确认：${error.message}。重新连接后刷新状态。`); }
  finally { hub.busy=false; }
}
function hubRow(title,body,state) {
  const row=document.createElement("div"); row.className="hub-row"; if(state)row.dataset.state=state;
  const strong=document.createElement("strong");strong.textContent=title;row.append(strong);
  const p=document.createElement("p");p.textContent=body;row.append(p);return row;
}
function hubButton(row,label,callback) { const b=document.createElement("button");b.type="button";b.textContent=label;b.addEventListener("click",callback);row.append(b); }
function hubSelect(id,items,kind) {
  const select=hubEl(id), focused=document.activeElement===select, previous=select.value;
  if(focused)return;
  select.replaceChildren();
  for(const item of items.filter(item=>item.kind===kind)) {
    const option=document.createElement("option");option.value=item.id;option.textContent=`${item.name}${item.selected?" · 当前输出/网络":""}${kind==="bluetooth"?item.connected?" · 已连接":" · 未连接":""}`;select.append(option);
  }
  if([...select.options].some(o=>o.value===previous))select.value=previous;
  else {
    const current=items.find(item=>item.kind===kind&&(kind==="bluetooth"?item.connected:item.selected));
    if(current)select.value=current.id;
  }
  if(!select.options.length) { const o=document.createElement("option");o.textContent="暂未提供，请在 Mac 设置检查";o.value="";select.append(o); }
}
function renderHub(sections) {
  hub.data=sections;
  const control=sections.control || [], current=control.find(item=>item.kind==="status") || {};
  if(!hub.volumeDragging && Number.isFinite(current.volume))hubEl("volume").value=Math.round(current.volume);
  hubEl("volume-value").textContent=Number.isFinite(current.volume)?`${Math.round(current.volume)}%`:"暂未提供";
  document.querySelectorAll("[data-hub-volume]").forEach(b=>b.disabled=!current.volume_supported);
  hubEl("volume").disabled=!current.volume_supported;hubEl("mute").disabled=!current.mute_supported;
  hub.muted=current.muted===true;hubEl("mute").textContent=hub.muted?"取消静音":"静音";
  hubEl("volume-reason").textContent=current.reason || "";
  hubSelect("audio",control,"audio");hubSelect("bluetooth",control,"bluetooth");hubSelect("wifi",control,"wifi");
  hubEl("permissions").textContent=control.filter(item=>item.kind==="permission").map(item=>`${item.name}：${item.reason}`).join("；");
  for(const id of ["cards","tasks","usage"])hubEl(id).replaceChildren();
  for(const c of sections.cards || []) {
    const row=hubRow(c.title,`${c.pinned?"已固定":`到期 ${new Date(c.expires_at*1000).toLocaleTimeString()}`}\n${c.body || c.url || ""}`);
    hubButton(row,c.pinned?"取消固定":"固定",async()=>{try{await hubRequest("cards",{operation:"pin",id:c.id,pinned:!c.pinned});await hubRefresh();}catch(e){hubMessage(e.message);}});
    hubButton(row,"清除",async()=>{try{await hubRequest("cards",{operation:"clear",id:c.id});await hubRefresh();}catch(e){hubMessage(e.message);}});
    if(c.kind==="file")hubButton(row,"打开 Mac 分享面板",()=>hubAction("file_share",c.file_ref));
    hubEl("cards").append(row);
  }
  for(const t of sections.tasks || []) {
    const row=hubRow(t.title,`${t.source}${t.session?" 会话":""} · ${hubState[t.state] || t.state} · ${new Date(t.updated_at*1000).toLocaleTimeString()}\n${t.summary || ""}`,t.state);
    if(!t.session)hubButton(row,t.viewed?"已查看":"标记已查看",()=>hubAction("task_viewed",t.id));
    if(t.has_details)hubButton(row,"打开详情",()=>hubAction("details_open",t.id));
    hubEl("tasks").append(row);
  }
  for(const u of sections.usage || []) {
    const window=u.window_minutes===300?"五小时":u.window_minutes===10080?"周":u.window_minutes?`${u.window_minutes} 分钟`:"窗口暂未提供";
    const valid=Number.isFinite(u.used_percent), state=u.state==="stale"?"已过期":hubState[u.state] || u.state;
    const provider=u.provider==="claude"?"Claude Code":u.provider==="codex"?"Codex":u.provider;
    const row=hubRow(`${provider} · ${window}`,valid?`剩余 ${(100-u.used_percent).toFixed(1)}% · 已用 ${u.used_percent.toFixed(1)}%\n${state} · 重置 ${u.resets_at?new Date(u.resets_at*1000).toLocaleString():"暂未提供"}`:u.provider==="claude"&&u.state==="not_provided"?"等待 Claude 采集器接入":state,u.state);
    if(valid){const progress=document.createElement("progress");progress.max=100;progress.value=100-u.used_percent;progress.setAttribute("aria-label",`${u.provider} ${window}剩余额度`);row.append(progress);}
    const stamp=document.createElement("p");stamp.textContent=u.observed_at?`读取时间 ${new Date(u.observed_at*1000).toLocaleTimeString()}`:"尚未读取";row.append(stamp);hubEl("usage").append(row);
  }
  if(!(sections.cards || []).length)hubEl("cards").append(hubRow("还没有信息卡","创建文字、链接或选择待分享文件。"));
  if(!(sections.tasks || []).length)hubEl("tasks").append(hubRow("还没有已登记任务","在 Mac 终端使用 task-run 运行命令。"));
}
async function hubRefresh() {
  try {
    const names=["control","cards","tasks","usage"];
    const values=await Promise.all(names.map(name=>hubRequest(name)));
    renderHub(Object.fromEntries(names.map((name,index)=>[name,values[index][name]])));
  } catch(e) { hubMessage(e.message); }
}
hubEl("volume").addEventListener("input",()=>{hub.volumeDragging=true;hubEl("volume-value").textContent=`${hubEl("volume").value}%（松手提交）`;});
hubEl("volume").addEventListener("change",()=>{hub.volumeDragging=false;hubAction("volume_set","",Number(hubEl("volume").value));});
document.querySelectorAll("[data-hub-volume]").forEach(b=>b.addEventListener("click",()=>hubAction("volume_set","",Number(b.dataset.hubVolume))));
hubEl("mute").addEventListener("click",()=>hubAction("mute_set","",!hub.muted));
hubEl("audio-go").addEventListener("click",()=>hubAction("audio_select",hubEl("audio").value));
hubEl("bt-connect").addEventListener("click",()=>hubAction("bluetooth_connect",hubEl("bluetooth").value));
hubEl("bt-disconnect").addEventListener("click",()=>hubAction("bluetooth_disconnect",hubEl("bluetooth").value));
hubEl("wifi-go").addEventListener("click",()=>{if(confirm("切换 Mac 网络可能断开 Bridge。密码或认证需要在 Mac 完成。确认切换已保存网络？"))hubAction("wifi_select",hubEl("wifi").value);});
hubEl("airdrop").addEventListener("click",()=>hubAction("airdrop_open"));
hubEl("control-refresh").addEventListener("click",()=>hubAction("control_refresh"));
hubEl("usage-refresh").addEventListener("click",()=>hubAction("usage_refresh"));
hubEl("card-kind").addEventListener("change",()=>{const kind=hubEl("card-kind").value;hubEl("card-body").hidden=kind==="file";hubEl("card-body-label").textContent=kind==="link"?"HTTP / HTTPS 链接":"正文";hubEl("card-body-label").hidden=kind==="file";hubEl("file-note").hidden=kind!=="file";});
hubEl("card-form").addEventListener("submit",async(event)=>{
  event.preventDefault();const kind=hubEl("card-kind").value, title=hubEl("card-title").value, content=hubEl("card-body").value;
  if(new TextEncoder().encode(title).length>48 || new TextEncoder().encode(content).length>512){hubMessage("标题最多 48 UTF-8 字节，正文或链接最多 512 字节。");return;}
  const payload={kind,title,ttl_minutes:Number(hubEl("card-expiry").value)};
  if(kind==="file"){delete payload.kind;payload.operation="file_pick";}else payload[kind==="link"?"url":"body"]=content;
  try {const result=await hubRequest("cards",payload);if(result.operation_id){hubMessage("请在 Mac 原生窗口选择文件");await hubPoll(result.operation_id);}else{hubMessage("信息卡已保存，已配对小屏幕会自动同步");await hubRefresh();}}catch(e){hubMessage(e.message);}
});
hubRefresh().then(()=>{if(hubEl("feedback").textContent==="正在读取桌面工具…")hubMessage("桌面工具已连接");});
setInterval(()=>{if(!document.hidden)hubRefresh();},5000);
setInterval(()=>{if(!document.hidden && !hub.busy && hubEl("usage-box").getBoundingClientRect().top<innerHeight && hubEl("usage-box").getBoundingClientRect().bottom>0)hubAction("usage_refresh");},60000);
