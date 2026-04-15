module.exports = {
  uiPort: process.env.PORT || 1880,
  flowFile: process.env.FLOWS || "flows.json",
  credentialSecret: process.env.NODE_RED_CREDENTIAL_SECRET || "home-esp8266-secret",
  diagnostics: {
    enabled: false,
    ui: false
  },
  editorTheme: {
    projects: {
      enabled: false
    }
  },
  contextStorage: {
    default: {
      module: "localfilesystem"
    }
  }
};
