const bcrypt = require("bcryptjs");

const adminUser = process.env.NODE_RED_ADMIN_USERNAME || "admin";
const adminPassword = process.env.NODE_RED_ADMIN_PASSWORD || "123456";
const adminAuthEnabled = (process.env.NODE_RED_ENABLE_ADMIN_AUTH || "true") !== "false";
const dashboardBufferSize = Number(process.env.NODE_RED_MAX_HTTP_BUFFER_SIZE || 8 * 1024 * 1024);

module.exports = {
  uiPort: process.env.PORT || 1880,
  flowFile: process.env.FLOWS || "flows.json",
  credentialSecret: process.env.NODE_RED_CREDENTIAL_SECRET || "home-esp8266-secret",
  httpStatic: "/data/public",
  dashboard: {
    maxHttpBufferSize: Number.isFinite(dashboardBufferSize) ? dashboardBufferSize : 8 * 1024 * 1024
  },
  adminAuth: adminAuthEnabled ? {
    type: "credentials",
    users: [
      {
        username: adminUser,
        password: bcrypt.hashSync(adminPassword, 8),
        permissions: "*"
      }
    ]
  } : undefined,
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
