/*
 * OptiScaler plugin API
 *
 * Lets an ASI plugin loaded by OptiScaler (plugins folder, LoadAsiPlugins=true) add its own tabs to the
 * OptiScaler menu and keep its settings in OptiScaler.ini.
 *
 * The API is plain C and does not depend on the ImGui version used by OptiScaler, so a plugin built against
 * this header keeps working with newer OptiScaler builds. No OptiScaler library or ImGui is linked.
 *
 * Usage:
 *   Export OptiPluginInit from the plugin. OptiScaler calls it once, right after InitializeASI (if present).
 *   Keep the api pointer, it stays valid for the lifetime of the process.
 *
 * Compatibility rules:
 *   - Fields are only ever appended to OptiPluginApi, never removed or reordered.
 *   - api->size is sizeof(OptiPluginApi) of the running OptiScaler. Before using a field added after
 *     version 1, check it with OPTI_API_HAS(api, Field).
 *
 * Strings are UTF-8 and copied by OptiScaler when it has to keep them.
 *
 * Threading:
 *   - Widget functions only work inside a tab draw callback, which runs on the render thread while the menu
 *     is open. Outside of it they do nothing and return false.
 *   - Draw callbacks have to be quick and must not call into D3D/Vulkan or present.
 *   - Registration, settings and logging functions can be called from any thread.
 *
 * Settings:
 *   Stored in OptiScaler.ini, in the [Plugin.<name>] section. <name> is the plugin file name without the
 *   extension and without a "-loadlate" suffix. A missing key or the value "auto" returns the default.
 *   Set* only changes the value in memory, SaveSettings (or Save INI in the menu) writes the file.
 */

#ifndef OPTISCALER_PLUGIN_H
#define OPTISCALER_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define OPTI_PLUGIN_API_VERSION 5

    // Handle of the calling plugin, given to OptiPluginInit
    typedef struct OptiPlugin OptiPlugin;

    // Called every frame while the tab is selected and the menu is open
    typedef void (*OptiDrawTabFn)(void* userData);

    // A Streamline interposer function the plugin handles (RegisterStreamlineRoutes)
    typedef struct OptiStreamlineRoute
    {
        const char* exportName; // the function's export name in the interposer
        void* handler;          // called instead of the function, same signature
        void** next;            // receives the function to call to continue, NULL while not installed
    } OptiStreamlineRoute;

    // OptiScaler hooked a Streamline interposer module (file version major.minor.patch)
    typedef void (*OptiStreamlineHookedFn)(void* interposerModule, uint32_t major, uint32_t minor, uint32_t patch,
                                           void* userData);

    // Read access to the game's NGX parameter block for one call (RegisterNgxRayReconstruction). Keys are the
    // block's own key strings. Each getter returns false when the key is not set (the output is left unchanged).
    typedef struct OptiNgxParams
    {
        void* block; // the game's parameter block, opaque
        bool (*getResource)(void* block, const char* key, void** resource); // ID3D12Resource* (DX12)
        bool (*getPointer)(void* block, const char* key, void** value);     // plain pointer values (matrices etc.)
        bool (*getUInt)(void* block, const char* key, uint32_t* value);
        bool (*getInt)(void* block, const char* key, int32_t* value);
        bool (*getFloat)(void* block, const char* key, float* value);
    } OptiNgxParams;

    // A plugin that provides ray reconstruction to games calling NGX directly (DX12). Calls arrive on the game's
    // thread inside its NGX call; the command list is the game's own (record into it, do not close or execute it).
    typedef struct OptiNgxRayReconstruction
    {
        uint32_t size;  // sizeof(OptiNgxRayReconstruction)
        void* userData; // passed back to every function
        // The game creates the ray reconstruction feature. Return false to refuse (the game sees a failure).
        bool (*create)(void* userData, void* d3d12Device, void* commandList, const OptiNgxParams* params,
                       void** context);
        // One evaluation: read the inputs from params, record the work into commandList, write the output resource.
        bool (*evaluate)(void* userData, void* context, void* commandList, const OptiNgxParams* params);
        // The game released the feature (or OptiScaler shuts down).
        void (*release)(void* userData, void* context);
    } OptiNgxRayReconstruction;

    typedef enum OptiLogLevel
    {
        OptiLog_Debug = 0,
        OptiLog_Info = 1,
        OptiLog_Warn = 2,
        OptiLog_Error = 3,
    } OptiLogLevel;

    typedef struct OptiPluginApi
    {
        uint32_t size;    // sizeof(OptiPluginApi) of the running OptiScaler
        uint32_t version; // OPTI_PLUGIN_API_VERSION of the running OptiScaler

        // Registration -------------------------------------------------------------------------------------

        // Adds a tab under the PLUGINS group of the menu sidebar. A plugin can register several tabs.
        // description is shown as the sidebar tooltip and can be NULL.
        bool (*RegisterTab)(OptiPlugin* plugin, const char* name, const char* description, OptiDrawTabFn draw,
                            void* userData);

        // Layout, only inside a draw callback ---------------------------------------------------------------

        // Starts a new card with a title, the previous one is closed. Cards are closed after the callback.
        void (*Section)(const char* title);
        void (*Separator)(void);
        // Places the next widget on the same line as the previous one
        void (*SameLine)(void);
        void (*Spacing)(void);
        // Widgets after Disabled(true) can't be interacted with, until Disabled(false) or the end of the callback
        void (*Disabled)(bool disabled);
        // Shows a tooltip when the previous widget is hovered
        void (*ItemTooltip)(const char* text);

        // Text, only inside a draw callback -----------------------------------------------------------------

        void (*Text)(const char* text);
        void (*TextDisabled)(const char* text);
        // r, g, b, a in 0.0 - 1.0
        void (*TextColored)(float r, float g, float b, float a, const char* text);

        // Widgets, only inside a draw callback --------------------------------------------------------------
        // Labels have to be unique inside a tab. Text after "##" is not shown, "Scale##2" is drawn as "Scale".
        // format is printf-like for the value shown on the slider, NULL for the default.

        bool (*Button)(const char* label);                // true when clicked
        bool (*Checkbox)(const char* label, bool* value); // true when changed
        bool (*SliderFloat)(const char* label, float* value, float min, float max, const char* format);
        bool (*SliderInt)(const char* label, int32_t* value, int32_t min, int32_t max, const char* format);
        // items is an array of itemCount strings, current is the selected index
        bool (*Combo)(const char* label, int32_t* current, const char* const* items, int32_t itemCount);
        // buffer is a zero terminated UTF-8 string that is edited in place
        bool (*InputText)(const char* label, char* buffer, size_t bufferSize);
        // rgba is float[4] in 0.0 - 1.0
        bool (*ColorEdit)(const char* label, float* rgba, bool withAlpha);

        // Menu state ----------------------------------------------------------------------------------------

        // UI scale of the menu, multiply custom sizes by it
        float (*GetMenuScale)(void);
        bool (*IsMenuOpen)(void);

        // Settings in [Plugin.<name>] of OptiScaler.ini ---------------------------------------------------

        bool (*GetBool)(OptiPlugin* plugin, const char* key, bool defaultValue);
        int32_t (*GetInt)(OptiPlugin* plugin, const char* key, int32_t defaultValue);
        float (*GetFloat)(OptiPlugin* plugin, const char* key, float defaultValue);
        // Copies the value to buffer (always zero terminated, truncated if needed).
        // Returns false and copies defaultValue when the key is missing.
        bool (*GetString)(OptiPlugin* plugin, const char* key, char* buffer, size_t bufferSize,
                          const char* defaultValue);

        void (*SetBool)(OptiPlugin* plugin, const char* key, bool value);
        void (*SetInt)(OptiPlugin* plugin, const char* key, int32_t value);
        void (*SetFloat)(OptiPlugin* plugin, const char* key, float value);
        void (*SetString)(OptiPlugin* plugin, const char* key, const char* value);
        // Removes the key, Get* returns the default afterwards
        void (*ResetValue)(OptiPlugin* plugin, const char* key);

        // Writes OptiScaler.ini to the disk
        bool (*SaveSettings)(OptiPlugin* plugin);

        // Misc ------------------------------------------------------------------------------------------------

        // Writes to the OptiScaler log, prefixed with the plugin name
        void (*Log)(OptiPlugin* plugin, OptiLogLevel level, const char* message);

        // Version 2 -------------------------------------------------------------------------------------------

        // Adds a card at the top of the Custom tab (the tab the menu opens on), shown whatever state the
        // upscaler is in. The callback draws the card's content, OptiScaler draws the card with its title.
        bool (*RegisterCustomCard)(OptiPlugin* plugin, const char* title, OptiDrawTabFn draw, void* userData);
        // An upscaler the plugin runs itself, outside of OptiScaler's upscalers (ray reconstruction for example):
        // its name and the GPU time of its last frame in ms. Shown in the FPS overlay and the menu while none of
        // OptiScaler's own upscalers is in use. Call it once per frame from any thread; a name that isn't reported
        // for half a second is no longer shown.
        void (*ReportUpscaler)(OptiPlugin* plugin, const char* name, float gpuTimeMs);

        // Version 3 -------------------------------------------------------------------------------------------

        // Streamline functions a plugin handles itself (a plugin that provides a Streamline feature, ray
        // reconstruction for example). Each route names a function of the game's Streamline 2.x interposer by its
        // export name; OptiScaler installs the plugin's handler in front of that function right after it hooks the
        // interposer (at once when the interposer is already hooked) and stores in *next the
        // function to call to continue (OptiScaler's own handling, if any, then Streamline). *next is NULL while
        // the route is not installed. The routes are removed when OptiScaler unhooks the interposer and installed
        // again on the next one. The route array is copied; next must stay valid for the lifetime of the process.
        // Returns false if a route is invalid (nothing is kept) or the plugin already registered routes.
        bool (*RegisterStreamlineRoutes)(OptiPlugin* plugin, const OptiStreamlineRoute* routes, uint32_t count);
        // Called each time OptiScaler hooked a Streamline 2.x interposer (module handle and its file version), before
        // the routes are installed, and at once if one is already hooked. A plugin that supports only some
        // Streamline versions registers its routes from this callback.
        bool (*SetStreamlineCallback)(OptiPlugin* plugin, OptiStreamlineHookedFn hooked, void* userData);

        // Version 4 -------------------------------------------------------------------------------------------

        // Ray reconstruction for games that call NGX directly (Unreal Engine's DLSS plugin, Nixxes ports): while a
        // provider is registered, OptiScaler reports ray reconstruction as available in the capability parameters and
        // sends the DX12 ray reconstruction feature's create / evaluate / release to it instead of its own handling.
        // The structure is copied. One provider per process; returns false if one is already registered.
        bool (*RegisterNgxRayReconstruction)(OptiPlugin* plugin, const OptiNgxRayReconstruction* provider);

        // Version 5 -------------------------------------------------------------------------------------------

        // Work a plugin records into the game's own command list outside of OptiScaler's upscaler calls (a feature
        // it runs from a Streamline route, for example). Call Begin before the first command and End after the last
        // one, on the game's thread, always as a pair. In between OptiScaler does not record the list's bound state
        // as the game's; End puts back the game's root signatures (RestoreComputeSignature /
        // RestoreGraphicSignature) and, with ExtendedStateRestore, also its root arguments, descriptor heaps and
        // pipeline state, as the game last bound them. Games that do not rebind their state after such a call then
        // keep working. commandList is the native ID3D12GraphicsCommandList*. Pairs may nest; only the outermost
        // pair restores.
        void (*BeginExternalCommandListWork)(OptiPlugin* plugin, void* commandList);
        void (*EndExternalCommandListWork)(OptiPlugin* plugin, void* commandList);

    } OptiPluginApi;

// True when the running OptiScaler has the given field of OptiPluginApi
#define OPTI_API_HAS(api, field)                                                                                       \
    ((api) != NULL && (api)->size >= offsetof(OptiPluginApi, field) + sizeof((api)->field) && (api)->field != NULL)

    // Exported by the plugin. Return false to not use the API (nothing registered is kept).
    typedef bool (*PFN_OptiPluginInit)(const OptiPluginApi* api, OptiPlugin* plugin);

#define OPTI_PLUGIN_INIT_NAME "OptiPluginInit"

#ifdef __cplusplus
}
#endif

#endif // OPTISCALER_PLUGIN_H
