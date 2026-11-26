// Injected at build time from the repo's module.prop (see vite.config.js), so
// the number shown in the UI can never drift from the module's real version.
export const YAY_VERSION = typeof __YAY_VERSION__ !== 'undefined' ? __YAY_VERSION__ : 'dev'
