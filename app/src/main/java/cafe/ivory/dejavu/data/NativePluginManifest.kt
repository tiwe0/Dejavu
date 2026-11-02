package cafe.ivory.dejavu.data

// All plugin types share the same metadata and declared-resource contract.
// NativePluginLibrary transports binary resources; Lua packages embed UTF-8.
typealias NativePluginManifest = PluginPackageManifest
