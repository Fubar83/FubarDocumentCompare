// Typer och anrop mot webbtjänstens första API (web/server/Legacy/LegacyEndpoints.cs). Uppräkningar kommer som strängar.

export type FindingKind = "Missing" | "Added" | "Changed" | "Moved" | "Color" | "Shape" | "Barcode" | "Braille"
  | "Uncertain" | "Style" | "StyleSystematic" | "PageNumber" | "Other";
export type Certainty = "Confirmed" | "High" | "Probable" | "Possible";
export type Severity = "Info" | "Minor" | "Major" | "Critical";
export type LabelStatus = "Pass" | "Warn" | "Fail";
export type DataStatus = "Found" | "Near" | "Missing" | "Mismatch";
export type ResultKind = "Label" | "Document" | "DataCheck";
export type EventType = "Progress" | "SheetLoaded" | "LabelLocated" | "FoldDetected" | "LabelAnalysed" | "Warning"
  | "Completed" | "Failed" | "Cancelled";

export interface Point { x: number; y: number; }

export interface JobEvent {
  type: EventType;
  stage: string;
  fraction: number;
  index: number;
  rotationDeg: number;
  corners: Point[];
  widthPx: number;
  heightPx: number;
  dpi: number;
  preliminary: boolean;
  labelStatus: LabelStatus;
  error: string;
  code?: string;
  message?: string;
}

export interface ReferenceInfo {
  id: string;
  name: string;
  widthMm: number;
  heightMm: number;
  layers: { name: string; defaultOn: boolean }[];
  spots: string[];
  barcodes: { symbology: string; data: string }[];
  tables: number;
  braille: string[];
}

export interface Finding {
  index: number;
  id: number;
  kind: FindingKind;
  severity: Severity;
  certainty: Certainty;
  score: number;
  systematic: boolean;
  label: number;
  labelCount: number;
  page: number;
  xMm: number; yMm: number; wMm: number; hMm: number;
  category: string;
  textFrom?: string;
  textTo?: string;
  detail: string;
  reasons: string[];
  fingerprint: string;
}

export interface LabelInfo {
  index: number; row: number; col: number;
  status: LabelStatus;
  rotationDeg: number;
  corners: Point[];
  folds: number;
  registrationRmsPx: number;
}

export interface DataItem {
  field: string; expected: string; status: DataStatus;
  foundText?: string; diff?: string; count: number; page: number;
}

export interface Summary {
  differences: number;
  labelsFound: number; labelsExpected: number; labelsPass: number; labelsWarn: number; labelsFail: number;
  dataFound: number; dataNear: number; dataMissing: number; dataMismatch: number;
  matchRatio: number;
}

export interface JobResult {
  id: string;
  mode: string;
  kind: ResultKind;
  summary: Summary;
  findings: Finding[];
  labels: LabelInfo[];
  dataItems: DataItem[];
}

async function check<T>(r: Response): Promise<T> {
  if (!r.ok) {
    let msg = `${r.status} ${r.statusText}`;
    try { msg = (await r.json()).error ?? msg; } catch { /* ingen JSON */ }
    throw new Error(msg);
  }
  return r.json() as Promise<T>;
}

export const api = {
  version: () => fetch("/api/version").then(r => check<{ version: string; api: number }>(r)),
  openReference: (form: FormData) => fetch("/api/references", { method: "POST", body: form }).then(r => check<ReferenceInfo>(r)),
  startCompare: (form: FormData) => fetch("/api/jobs/compare", { method: "POST", body: form }).then(r => check<{ id: string }>(r)),
  startDocuments: (form: FormData) => fetch("/api/jobs/documents", { method: "POST", body: form }).then(r => check<{ id: string }>(r)),
  startDataCheck: (form: FormData) => fetch("/api/jobs/data-check", { method: "POST", body: form }).then(r => check<{ id: string }>(r)),
  result: (id: string) => fetch(`/api/jobs/${id}/result`).then(r => check<JobResult>(r)),
  cancel: (id: string) => fetch(`/api/jobs/${id}`, { method: "DELETE" }),
  cropUrl: (id: string, i: number, side: "reference" | "sample", maxWidth = 480) =>
    `/api/jobs/${id}/crops/${i}/${side}?maxWidth=${maxWidth}`,
  reportUrl: (id: string) => `/api/jobs/${id}/report`,

  /** Jobbets händelser (hela förloppet). Returnerar en funktion som kopplar ner. */
  events(id: string, onEvent: (e: JobEvent) => void, onEnd: () => void): () => void {
    const src = new EventSource(`/api/jobs/${id}/events`);
    const types: EventType[] = ["Progress", "SheetLoaded", "LabelLocated", "FoldDetected", "LabelAnalysed", "Warning",
      "Completed", "Failed", "Cancelled"];
    for (const t of types) src.addEventListener(t, ev => onEvent(JSON.parse((ev as MessageEvent).data)));
    src.addEventListener("end", () => { src.close(); onEnd(); });
    return () => src.close();
  },
};
