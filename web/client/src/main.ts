// printcmp-webb: formulären, jobbets liveförlopp (SSE) och resultatvyn.
import { api, Certainty, DataItem, Finding, JobEvent, JobResult, LabelInfo, ReferenceInfo } from "./api.js";

const $ = <T extends HTMLElement = HTMLElement>(sel: string) => document.querySelector(sel) as T;
const el = <K extends keyof HTMLElementTagNameMap>(tag: K, props: Partial<HTMLElementTagNameMap[K]> = {}, ...kids: (Node | string)[]) => {
  const n = Object.assign(document.createElement(tag), props);
  n.append(...kids);
  return n;
};
const SVG = "http://www.w3.org/2000/svg";
const svg = (tag: string, attrs: Record<string, string | number>) => {
  const n = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, String(v));
  return n;
};

const CERT_RANK: Record<Certainty, number> = { Confirmed: 0, High: 1, Probable: 2, Possible: 3 };
const CERT_SV: Record<Certainty, string> = { Confirmed: "bekräftad", High: "hög", Probable: "trolig", Possible: "möjlig" };
const KIND_SV: Record<string, string> = {
  Missing: "Saknas", Added: "Tillagd", Changed: "Ändrad", Moved: "Flyttad", Color: "Färg", Shape: "Form",
  Barcode: "Streckkod", Braille: "Punktskrift", Uncertain: "Osäker", Style: "Stil", StyleSystematic: "Stil (systematisk)",
  PageNumber: "Sidnummer", Other: "Övrigt",
};
const STAGE_SV: Record<string, string> = {
  Render: "Renderar", Locate: "Hittar etiketter", Register: "Passar in", Normalize: "Normaliserar", Diff: "Jämför",
  Folds: "Letar veck", Classify: "Klassar", Aggregate: "Sammanställer", Text: "Läser text", DataCheck: "Kontrollerar data",
  Report: "Rapport", Done: "Klart",
};

// Ungefärlig andel av körtiden per steg (passningen dominerar för scannade ark); ger en jämn total.
const STAGE_WEIGHT: [string, number][] = [["Render", 5], ["Locate", 5], ["Register", 35], ["Normalize", 8], ["Diff", 17],
  ["Folds", 2], ["Classify", 17], ["Aggregate", 3], ["Text", 3], ["DataCheck", 3], ["Report", 2]];
const WEIGHT_SUM = STAGE_WEIGHT.reduce((a, [, w]) => a + w, 0);
function overall(stage: string, fraction: number): number {
  if (stage === "Done") return 1;
  let before = 0;
  for (const [s, w] of STAGE_WEIGHT) {
    if (s === stage) return (before + w * fraction) / WEIGHT_SUM;
    before += w;
  }
  return 0;
}

// ------------------------------------------------------------------ godkända fynd (per webbläsare)

const APPROVED_KEY = "printcmp.approved";
function loadApproved(): Set<string> {
  try { return new Set(JSON.parse(localStorage.getItem(APPROVED_KEY) ?? "[]")); } catch { return new Set(); }
}
const approved = loadApproved();
function saveApproved() {
  try { localStorage.setItem(APPROVED_KEY, JSON.stringify([...approved])); } catch { /* privat läge */ }
}

// ------------------------------------------------------------------ flikar

document.querySelectorAll<HTMLButtonElement>("[data-tab]").forEach(b => b.addEventListener("click", () => {
  document.querySelectorAll<HTMLButtonElement>("[data-tab]").forEach(x => x.setAttribute("aria-selected", String(x === b)));
  document.querySelectorAll<HTMLElement>("[data-panel]").forEach(p => (p.hidden = p.dataset.panel !== b.dataset.tab));
}));

api.version().then(v => ($("#version").textContent = `kärna ${v.version}`)).catch(() => { /* visas inte */ });

function showError(e: unknown) {
  const p = $("#error");
  p.textContent = e instanceof Error ? e.message : String(e);
  p.hidden = false;
}
function clearError() { $("#error").hidden = true; }

/** Formulärdata där kryssrutor alltid skickas (true/false) och tomma filfält utelämnas. */
function formData(form: HTMLFormElement): FormData {
  const fd = new FormData();
  for (const input of Array.from(form.elements) as HTMLInputElement[]) {
    if (!input.name) continue;
    if (input.type === "checkbox") fd.set(input.name, String(input.checked));
    else if (input.type === "file") { if (input.files?.[0]) fd.set(input.name, input.files[0]); }
    else if (input.value !== "") fd.set(input.name, input.value);
  }
  return fd;
}

async function submitting(form: HTMLFormElement, work: () => Promise<void>) {
  const btn = form.querySelector<HTMLButtonElement>("button[type=submit]")!;
  btn.disabled = true;
  clearError();
  try { await work(); } catch (e) { showError(e); } finally { btn.disabled = false; }
}

// ------------------------------------------------------------------ etikett: original → prov

let reference: ReferenceInfo | null = null;

$<HTMLFormElement>("#ref-form").addEventListener("submit", ev => {
  ev.preventDefault();
  const form = ev.target as HTMLFormElement;
  submitting(form, async () => {
    const fd = formData(form);
    // lager och dekorfärger som avmarkerats i den förra analysen utesluts
    const off = (name: string) => Array.from(form.querySelectorAll<HTMLInputElement>(`input[name=${name}]:not(:checked)`)).map(i => i.value);
    fd.delete("layer");
    fd.delete("spot");
    if (off("layer").length) fd.set("excludeLayers", off("layer").join(";"));
    if (off("spot").length) fd.set("excludeSpots", off("spot").join(";"));
    reference = await api.openReference(fd);
    renderReference(reference, new Set(off("layer")), new Set(off("spot")));
    $("#cmp-form").hidden = false;
  });
});

function renderReference(r: ReferenceInfo, offLayers: Set<string>, offSpots: Set<string>) {
  const box = $("#ref-info");
  box.hidden = false;
  box.replaceChildren(el("div", {}, el("strong", {}, r.name), ` – ${r.widthMm.toFixed(1)} × ${r.heightMm.toFixed(1)} mm`));
  const toggles = (title: string, name: string, items: { value: string; on: boolean }[]) => {
    if (!items.length) return;
    const toggle = (value: string, on: boolean) => {
      const input = document.createElement("input");
      Object.assign(input, { type: "checkbox", name, value, checked: on });
      return el("label", { className: "check" }, input, value);
    };
    box.append(el("div", {}, title), el("div", { className: "row" }, ...items.map(i => toggle(i.value, i.on))));
  };
  toggles("Lager (avmarkera och analysera om för att utesluta):", "layer",
    r.layers.map(l => ({ value: l.name, on: l.defaultOn && !offLayers.has(l.name) })));
  toggles("Dekorfärger:", "spot", r.spots.map(s => ({ value: s, on: !offSpots.has(s) })));
  const facts: string[] = [];
  if (r.barcodes.length) facts.push(`Streckkoder: ${r.barcodes.map(b => `${b.symbology} ${b.data}`).join(", ")}`);
  if (r.tables) facts.push(`Tabeller: ${r.tables}`);
  if (r.braille.length) facts.push(`Punktskrift: ${r.braille.join(" / ")}`);
  if (facts.length) box.append(el("ul", {}, ...facts.map(f => el("li", {}, f))));
  $("#ref-form").querySelector("button[type=submit]")!.textContent = "Analysera om";
}

$<HTMLFormElement>("#cmp-form").addEventListener("submit", ev => {
  ev.preventDefault();
  const form = ev.target as HTMLFormElement;
  submitting(form, async () => {
    if (!reference) throw new Error("Analysera originalet först.");
    const fd = formData(form);
    fd.set("referenceId", reference.id);
    const { id } = await api.startCompare(fd);
    watchJob(id, "Etikett / ark");
  });
});

$<HTMLFormElement>("#doc-form").addEventListener("submit", ev => {
  ev.preventDefault();
  const form = ev.target as HTMLFormElement;
  submitting(form, async () => watchJob((await api.startDocuments(formData(form))).id, "Dokument"));
});

$<HTMLFormElement>("#data-form").addEventListener("submit", ev => {
  ev.preventDefault();
  const form = ev.target as HTMLFormElement;
  submitting(form, async () => watchJob((await api.startDataCheck(formData(form))).id, "Datakontroll"));
});

// ------------------------------------------------------------------ jobbets förlopp

let disconnect: (() => void) | null = null;
let currentJob = "";
let progress = 0;

function watchJob(id: string, title: string) {
  disconnect?.();
  currentJob = id;
  $("#job").hidden = false;
  $("#job-title").textContent = title;
  $<HTMLButtonElement>("#cancel").hidden = false;
  $("#report").hidden = true;
  progress = 0;
  $("#bar").style.width = "0";
  $("#stage").textContent = "Startar …";
  for (const s of ["#log", "#summary", "#labels", "#data-items", "#findings"]) $(s).replaceChildren();
  $("#findings-tools").hidden = true;
  const sheet = $<SVGSVGElement & HTMLElement>("#sheet");
  sheet.replaceChildren();
  sheet.hidden = true;
  $("#job").scrollIntoView({ behavior: "smooth", block: "start" });

  disconnect = api.events(id, e => onEvent(e), () => finished(id));
}

$("#cancel").addEventListener("click", () => { if (currentJob) api.cancel(currentJob); });

function log(text: string, cls = "") {
  const ol = $("#log");
  ol.append(el("li", { className: cls }, text));
  ol.scrollTop = ol.scrollHeight;
}

function onEvent(e: JobEvent) {
  const sheet = $<SVGSVGElement & HTMLElement>("#sheet");
  switch (e.type) {
    case "Progress":
      progress = Math.max(progress, overall(e.stage, e.fraction));
      $("#bar").style.width = `${(progress * 100).toFixed(1)}%`;
      $("#stage").textContent = `${STAGE_SV[e.stage] ?? e.stage} – totalt ${Math.round(progress * 100)} %`;
      break;
    case "SheetLoaded":
      sheet.setAttribute("viewBox", `0 0 ${e.widthPx} ${e.heightPx}`);
      sheet.hidden = false;
      log(`Arket inläst: ${Math.round(e.widthPx)} × ${Math.round(e.heightPx)} px, ${Math.round(e.dpi)} dpi`);
      break;
    case "LabelLocated": {
      const pts = e.corners.map(p => `${p.x},${p.y}`).join(" ");
      sheet.append(svg("polygon", { points: pts, class: "label", id: `lbl-${e.index}` }));
      const c = e.corners.reduce((a, p) => ({ x: a.x + p.x / 4, y: a.y + p.y / 4 }), { x: 0, y: 0 });
      const ys = e.corners.map(p => p.y), xs = e.corners.map(p => p.x);
      const size = Math.min(Math.max(...ys) - Math.min(...ys), Math.max(...xs) - Math.min(...xs)) / 4;
      const t = svg("text", { x: c.x, y: c.y, "text-anchor": "middle", "dominant-baseline": "middle", "font-size": Math.max(12, size) });
      t.textContent = String(e.index);
      sheet.append(t);
      log(`Etikett ${e.index} hittad (${e.rotationDeg.toFixed(1)}°)`);
      break;
    }
    case "FoldDetected":
      sheet.append(svg("line", { x1: e.corners[0].x, y1: e.corners[0].y, x2: e.corners[1].x, y2: e.corners[1].y, class: "fold" }));
      log("Veck hittat", "warn");
      break;
    case "LabelAnalysed":
      document.getElementById(`lbl-${e.index}`)?.setAttribute("class", `label ${e.labelStatus}`);
      log(`Etikett ${e.index}: ${e.labelStatus}${e.preliminary ? " (preliminärt)" : ""}`, e.labelStatus === "Fail" ? "bad" : "");
      break;
    case "Warning":
      log(`Varning: ${e.message ?? e.code}`, "warn");
      break;
    case "Failed":
      log(`Fel: ${e.message}`, "bad");
      break;
    case "Cancelled":
      log("Avbrutet", "warn");
      break;
    case "Completed":
      $("#bar").style.width = "100%";
      break;
  }
}

async function finished(id: string) {
  $<HTMLButtonElement>("#cancel").hidden = true;
  if (id !== currentJob) return;
  try {
    const r = await api.result(id);
    $("#stage").textContent = "Klart";
    renderResult(r);
  } catch (e) {
    $("#stage").textContent = "";
    showError(e);
  }
}

// ------------------------------------------------------------------ resultat

function renderResult(r: JobResult) {
  const s = r.summary;
  const chips: [string, string][] = r.kind === "DataCheck" ? [] : [[`${s.differences} skillnader`, s.differences ? "Fail" : "Pass"]];
  if (r.kind === "Label" && s.labelsFound) {
    chips.push([`${s.labelsFound}${s.labelsExpected ? `/${s.labelsExpected}` : ""} etiketter`, ""]);
    chips.push([`${s.labelsPass} godkända`, "Pass"], [`${s.labelsWarn} varning`, "Warn"], [`${s.labelsFail} underkända`, "Fail"]);
  }
  if (r.kind === "Document") chips.push([`${(s.matchRatio * 100).toFixed(1)} % lika ord`, ""]);
  if (r.dataItems.length)
    chips.push([`data: ${s.dataFound} hittade`, "Found"], [`${s.dataNear} nära`, "Near"], [`${s.dataMissing} saknas`, "Missing"],
      [`${s.dataMismatch} fel`, "Mismatch"]);
  $("#summary").replaceChildren(...chips.map(([t, c]) => el("span", { className: `chip ${c}` }, t)));
  if (r.kind !== "DataCheck") {
    const a = $<HTMLAnchorElement>("#report");
    a.href = api.reportUrl(r.id);
    a.hidden = false;
  }
  renderLabels(r.labels);
  renderData(r.dataItems);
  renderFindings(r);
}

function renderLabels(labels: LabelInfo[]) {
  $("#labels").replaceChildren(...labels.map(l => el("span", {
    className: `chip ${l.status}`,
    title: `rad ${l.row}, kolumn ${l.col}, ${l.rotationDeg.toFixed(1)}°, passning ${l.registrationRmsPx.toFixed(2)} px` + (l.folds ? `, ${l.folds} veck` : ""),
  }, `#${l.index} ${l.status}`)));
}

function renderData(items: DataItem[]) {
  if (!items.length) { $("#data-items").replaceChildren(); return; }
  const rows = items.map(d => el("tr", {},
    el("td", {}, d.field),
    el("td", {}, d.expected),
    el("td", {}, el("span", { className: `chip ${d.status}` }, d.status)),
    el("td", {}, d.foundText ? `${d.foundText}${d.diff ? ` (${d.diff})` : ""}` : ""),
    el("td", {}, d.page ? `s. ${d.page}` : "")));
  $("#data-items").replaceChildren(el("div", { className: "table-wrap" }, el("table", {},
    el("thead", {}, el("tr", {}, ...["Fält", "Förväntat", "Status", "Hittat", "Sida"].map(h => el("th", {}, h)))),
    el("tbody", {}, ...rows))));
}

function renderFindings(r: JobResult) {
  const tools = $("#findings-tools");
  if (!r.findings.length) {
    tools.hidden = true;
    $("#findings").replaceChildren(el("p", { className: "muted" }, r.kind === "DataCheck" ? "" : "Inga skillnader."));
    return;
  }
  tools.hidden = false;
  const kindSel = $<HTMLSelectElement>("#f-kind");
  const kinds = [...new Set(r.findings.map(f => f.kind))];
  kindSel.replaceChildren(el("option", { value: "" }, "alla"), ...kinds.map(k => el("option", { value: k }, KIND_SV[k] ?? k)));
  const certSel = $<HTMLSelectElement>("#f-cert");
  const hideOk = $<HTMLInputElement>("#f-hide-ok");
  const draw = () => {
    const shown = r.findings.filter(f =>
      (!kindSel.value || f.kind === kindSel.value) &&
      CERT_RANK[f.certainty] <= Number(certSel.value) &&
      !(hideOk.checked && approved.has(f.fingerprint)));
    const hidden = r.findings.length - shown.length;
    $("#findings").replaceChildren(...shown.map(f => findingView(r, f, draw)),
      ...(hidden ? [el("p", { className: "muted" }, `${hidden} dolda av filtret`)] : []));
  };
  kindSel.onchange = certSel.onchange = hideOk.onchange = draw;
  draw();
}

function findingView(r: JobResult, f: Finding, redraw: () => void): HTMLElement {
  const where = r.kind === "Document" ? `s. ${f.page}` : f.label ? `etikett ${f.label}${f.labelCount > 1 ? ` (+${f.labelCount - 1})` : ""}` : "arket";
  const isOk = approved.has(f.fingerprint);
  const d = el("details", { className: `finding${isOk ? " approved" : ""}` },
    el("summary", {},
      el("span", { className: `kind sev-${f.severity}` }, KIND_SV[f.kind] ?? f.kind),
      el("span", {}, f.category),
      el("span", { className: "muted" }, `${where} · ${CERT_SV[f.certainty]} · ${f.score}`),
      ...(f.systematic ? [el("span", { className: "chip" }, "systematisk")] : []),
      el("span", {}, f.detail)));
  let filled = false;
  d.addEventListener("toggle", () => {
    if (!d.open || filled) return;
    filled = true;
    const body = el("div", { className: "body" });
    if (f.textFrom || f.textTo) body.append(el("div", { className: "text-diff" }, ...textDiff(f.textFrom ?? "", f.textTo ?? "")));
    if (r.kind === "Label") {
      const fig = (side: "reference" | "sample", cap: string) => el("figure", {},
        el("img", { src: api.cropUrl(r.id, f.index, side), alt: `${cap}, fynd ${f.id}`, loading: "lazy" }), el("figcaption", {}, cap));
      body.append(el("div", { className: "crops" }, fig("reference", "Original"), fig("sample", "Prov")));
    }
    if (f.reasons.length) body.append(el("ul", { className: "muted" }, ...f.reasons.map(x => el("li", {}, x))));
    const btn = el("button", { className: "secondary", type: "button" }, isOk ? "Ångra godkännande" : "Godkänn (ignorera framöver)");
    btn.onclick = () => {
      if (approved.has(f.fingerprint)) approved.delete(f.fingerprint); else approved.add(f.fingerprint);
      saveApproved();
      redraw();
    };
    body.append(el("div", { className: "row" }, btn, el("span", { className: "muted" }, `${f.xMm.toFixed(1)}, ${f.yMm.toFixed(1)} mm · ${f.fingerprint}`)));
    d.append(body);
  });
  return d;
}

/** Ordvis skillnad för visning (enkel LCS). */
function textDiff(a: string, b: string): Node[] {
  const x = a.split(/(\s+)/), y = b.split(/(\s+)/);
  const n = x.length, m = y.length;
  if (n * m > 400_000) return [el("del", {}, a), document.createTextNode(" → "), el("ins", {}, b)];
  const L = Array.from({ length: n + 1 }, () => new Uint16Array(m + 1));
  for (let i = n - 1; i >= 0; i--)
    for (let j = m - 1; j >= 0; j--)
      L[i][j] = x[i] === y[j] ? L[i + 1][j + 1] + 1 : Math.max(L[i + 1][j], L[i][j + 1]);
  const out: Node[] = [];
  let i = 0, j = 0;
  while (i < n || j < m) {
    if (i < n && j < m && x[i] === y[j]) { out.push(document.createTextNode(x[i])); i++; j++; }
    else if (j < m && (i === n || L[i][j + 1] >= L[i + 1][j])) out.push(el("ins", {}, y[j++]));
    else out.push(el("del", {}, x[i++]));
  }
  return out;
}
