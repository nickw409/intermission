/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** "1" serves the API from contract fixtures via MSW (dev server only). */
  readonly VITE_MOCK?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
