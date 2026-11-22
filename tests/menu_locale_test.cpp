// The menu's translations without a game or a GPU: ImGui runs on the CPU, the draw data is rasterized here.
//
//   menu_locale_test.exe [out folder for the pictures]
//
// Checks fallback to English, the language match, that every glyph of every table is in the fonts of its language, that
// a widget's ID is the same in every language, and that no entry is dropped as unsafe. Each language draws a sample
// panel; the pictures are for looking at (RGBA, width and height in the first two 32-bit words of the .rgba file).
#define NOMINMAX
#include <windows.h>

#include <menu/i18n/MenuLocaleImGui.h>
#include <menu/i18n/MenuLocaleTables.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

static void checkEq(std::string_view a, std::string_view b, const char* what)
{
    if (a != b)
    {
        std::printf("FAIL: %s: [%.*s] vs [%.*s]\n", what, (int) a.size(), a.data(), (int) b.size(), b.data());
        failures++;
    }
}

static void pumpTextures()
{
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
    {
        if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
        {
            tex->SetTexID((ImTextureID) 1);
            tex->SetStatus(ImTextureStatus_OK);
        }
        else if (tex->Status == ImTextureStatus_WantDestroy)
        {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

static void useLanguage(const char* code)
{
    MenuLocale::Request(code);
    MenuLocale::ApplyPending(ImGui::GetIO().Fonts);
}

static void testUnits()
{
    using namespace MenuLocale;

    check(TranslationIsSafe("%d frames", "%d quadros"), "plain conversion");
    check(!TranslationIsSafe("%d frames", "%s quadros"), "other conversion");
    check(!TranslationIsSafe("%d of %s", "%s de %d"), "reordered conversions");
    check(!TranslationIsSafe("100% sure", "100% seguro"), "stray conversion from a percent sign");
    check(TranslationIsSafe("50%% up", "50%% acima"), "escaped percent");
    check(TranslationIsSafe("about 8%)", "cerca de 8%)"), "percent in prose");
    check(!TranslationIsSafe("half of it", "50%, a metade"), "percent in prose the English does not have");
    check(!TranslationIsSafe("on %u%% of frames", "em %u% (dos quadros)"), "lost escape of a percent");
    check(TranslationIsSafe("13% faster", "%13 daha"), "percent before the number");
    check(!TranslationIsSafe("x {}", "x"), "lost braces");
    check(!TranslationIsSafe("x", "y##z"), "id in a translation");

    checkEq(MatchLanguage("pt-PT"), Tables::Has("pt-BR") ? "pt-BR" : "en", "pt-PT");
    checkEq(MatchLanguage("en-GB"), "en", "en-GB");
    checkEq(MatchLanguage("xx-YY"), "en", "unknown locale");
    checkEq(MatchLanguage(""), "en", "empty locale");
    if (Tables::Has("zh-CN"))
        checkEq(MatchLanguage("zh-SG"), "zh-CN", "zh-SG");
    if (Tables::Has("zh-TW"))
    {
        checkEq(MatchLanguage("zh-Hant-HK"), "zh-TW", "zh-Hant-HK");
        checkEq(MatchLanguage("zh-TW"), "zh-TW", "zh-TW");
    }
    if (Tables::Has("es"))
        checkEq(MatchLanguage("es-419"), "es", "es-419");
    std::printf("Windows UI language maps to %s\n", DetectSystemLanguage());
}

// ID of a label as ImGui computes it under an empty stack.
static ImGuiID idOf(const char* label) { return ImHashStr(MenuLocale::Label(label), 0, 0); }

static void testIds(const char* code, std::vector<ImGuiID>& plain, std::vector<ImGuiID>& withId)
{
    const bool first = plain.empty();
    size_t i = 0;
    for (size_t k = 0; k < MenuLocale::Tables::KeyCount(); k++)
    {
        const char* key = MenuLocale::Tables::Key(k);
        const ImGuiID a = idOf(key);
        const ImGuiID b = idOf((std::string(key) + "##row3").c_str());
        if (first)
        {
            plain.push_back(a);
            withId.push_back(b);
        }
        else if (plain[i] != a || withId[i] != b)
        {
            std::printf("FAIL: %s: ID changes for [%s]\n", code, key);
            failures++;
        }
        i++;

        // What ImGui shows of the label is the translation, with no piece of the ID.
        const char* label = MenuLocale::Label(key);
        const char* end = ImGui::FindRenderedTextEnd(label);
        checkEq(std::string_view(label, end - label), MenuLocale::Text(key), "shown text");
    }

    const ImGuiID own = idOf("Off###own");
    static ImGuiID ownFirst = own;
    check(own == ownFirst, "### id");
    check(strcmp(MenuLocale::Label("##hidden"), "##hidden") == 0, "bare id stays");
}

// Glyphs missing from the fonts in this language's table.
static size_t testGlyphs(const char* code)
{
    size_t count = 0;
    const MenuLocale::Tables::Entry* entries = MenuLocale::Tables::Find(code, count);
    ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
    std::vector<unsigned int> missing;

    for (size_t i = 0; i < count; i++)
    {
        for (const char* at = entries[i].text; *at != 0;)
        {
            unsigned int c = 0;
            at += ImTextCharFromUtf8(&c, at, nullptr);
            if (c < 32 || c == 0x2029)
                continue;
            if (!font->IsGlyphInFont((ImWchar) c) && std::find(missing.begin(), missing.end(), c) == missing.end())
                missing.push_back(c);
        }
    }

    for (unsigned int c : missing)
        std::printf("FAIL: %s: no font has U+%04X\n", code, c);
    failures += (int) missing.size();
    return missing.size();
}

static void drawSample()
{
    static bool ticked = true;
    static float value = 0.5f;
    static int choice = 1;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Menu", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::SeparatorText("Upscalers");
    ImGui::Checkbox("Frame Generation", &ticked);
    ImGui::SameLine();
    ImGui::Button("Save Settings");
    ImGui::SameLine();
    ImGui::Button("Close");
    ImGui::SliderFloat("Sharpness", &value, 0.0f, 1.0f);
    const char* const items[] = { "Quality", "Balanced", "Performance", "Ultra Performance" };
    ImGui::Combo("Upscaler Quality", &choice, items, 4);
    ImGui::Text("Performance");
    ImGui::Text("%s", "Neural Rendering");
    ImGui::TextDisabled("(restart to apply)");
    ImGui::Text("Frame Time: %7.2f ms, Avg: %7.2f ms", 16.6f, 16.9f);
    ImGui::TextWrapped("After changing this option, please Save Settings.");
    ImGui::TextUnformatted(MenuLocale::Text("\n\nAfter changing this option, please Save Settings.\n"
                                            "It will be applied on next launch."));
    ImGui::End();
}

static uint8_t mul8(int a, int b) { return (uint8_t) ((a * b + 127) / 255); }

static void rasterize(const ImDrawData* data, std::vector<uint32_t>& pixels, int width, int height)
{
    pixels.assign((size_t) width * height, 0xFF242424);

    for (const ImDrawList* list : data->CmdLists)
    {
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            ImTextureData* tex = cmd.TexRef._TexData;
            const ImVec4 clip = cmd.ClipRect;
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3)
            {
                const ImDrawVert& a = list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset + i] + cmd.VtxOffset];
                const ImDrawVert& b = list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset + i + 1] + cmd.VtxOffset];
                const ImDrawVert& c = list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset + i + 2] + cmd.VtxOffset];

                const float det = (b.pos.y - c.pos.y) * (a.pos.x - c.pos.x) + (c.pos.x - b.pos.x) * (a.pos.y - c.pos.y);
                if (std::fabs(det) < 1e-6f)
                    continue;

                const int x0 = (int) std::max(std::floor(std::min({ a.pos.x, b.pos.x, c.pos.x })), std::max(0.0f, clip.x));
                const int x1 = (int) std::min(std::ceil(std::max({ a.pos.x, b.pos.x, c.pos.x })), std::min((float) width, clip.z));
                const int y0 = (int) std::max(std::floor(std::min({ a.pos.y, b.pos.y, c.pos.y })), std::max(0.0f, clip.y));
                const int y1 = (int) std::min(std::ceil(std::max({ a.pos.y, b.pos.y, c.pos.y })), std::min((float) height, clip.w));

                for (int y = y0; y < y1; y++)
                {
                    for (int x = x0; x < x1; x++)
                    {
                        const float px = x + 0.5f, py = y + 0.5f;
                        const float wa = ((b.pos.y - c.pos.y) * (px - c.pos.x) + (c.pos.x - b.pos.x) * (py - c.pos.y)) / det;
                        const float wb = ((c.pos.y - a.pos.y) * (px - c.pos.x) + (a.pos.x - c.pos.x) * (py - c.pos.y)) / det;
                        const float wc = 1.0f - wa - wb;
                        if (wa < 0 || wb < 0 || wc < 0)
                            continue;

                        const float u = wa * a.uv.x + wb * b.uv.x + wc * c.uv.x;
                        const float v = wa * a.uv.y + wb * b.uv.y + wc * c.uv.y;
                        const int tx = std::clamp((int) (u * tex->Width), 0, tex->Width - 1);
                        const int ty = std::clamp((int) (v * tex->Height), 0, tex->Height - 1);
                        const uint8_t* texel = (const uint8_t*) tex->GetPixelsAt(tx, ty);
                        const int ta = tex->Format == ImTextureFormat_Alpha8 ? texel[0] : texel[3];
                        const int tr = tex->Format == ImTextureFormat_Alpha8 ? 255 : texel[0];
                        const int tg = tex->Format == ImTextureFormat_Alpha8 ? 255 : texel[1];
                        const int tb = tex->Format == ImTextureFormat_Alpha8 ? 255 : texel[2];

                        const ImU32 col = a.col;
                        const int sa = mul8((col >> 24) & 255, ta);
                        const int sr = mul8(col & 255, tr), sg = mul8((col >> 8) & 255, tg), sb = mul8((col >> 16) & 255, tb);

                        uint32_t& out = pixels[(size_t) y * width + x];
                        const int dr = out & 255, dg = (out >> 8) & 255, db = (out >> 16) & 255;
                        const int r = (sr * sa + dr * (255 - sa)) / 255;
                        const int g = (sg * sa + dg * (255 - sa)) / 255;
                        const int bl = (sb * sa + db * (255 - sa)) / 255;
                        out = 0xFF000000u | (bl << 16) | (g << 8) | r;
                    }
                }
            }
        }
    }
}

static void renderPicture(const char* code, const std::filesystem::path& folder)
{
    ImGuiIO& io = ImGui::GetIO();
    const int width = 640, height = 300;
    io.DisplaySize = ImVec2((float) width, (float) height);
    io.DeltaTime = 1.0f / 60.0f;

    // Two frames: the first asks for the glyphs and the layout settles on the second.
    for (int frame = 0; frame < 3; frame++)
    {
        ImGui::NewFrame();
        drawSample();
        ImGui::Render();
        pumpTextures();
    }

    std::vector<uint32_t> pixels;
    rasterize(ImGui::GetDrawData(), pixels, width, height);

    std::ofstream file(folder / (std::string(code) + ".rgba"), std::ios::binary);
    const int32_t size[2] = { width, height };
    file.write((const char*) size, sizeof(size));
    file.write((const char*) pixels.data(), pixels.size() * 4);
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontFromFileTTF((std::filesystem::path(__FILE__).parent_path().parent_path() /
                                  "OptiScaler/menu/font/Hack-Regular.ttf").string().c_str(), 14.0f);

    // Before any request nothing changes and no table is needed.
    checkEq(MenuLocale::Text("Save Settings"), "Save Settings", "text before a language");
    checkEq(MenuLocale::Label("Save Settings"), "Save Settings", "label before a language");
    checkEq(MenuLocale::ActiveCode(), "en", "code before a language");

    testUnits();

    const std::filesystem::path folder = argc > 1 ? argv[1] : "";
    if (!folder.empty())
        std::filesystem::create_directories(folder);

    std::vector<ImGuiID> plain, withId;
    const size_t keys = MenuLocale::Tables::KeyCount();
    std::printf("%zu keys\n%-7s %-6s %-6s %s\n", keys, "lang", "have", "kept", "fonts");

    const int languages = argc > 2 ? atoi(argv[2]) : MenuLocale::LanguageCount();
    const bool frames = argc <= 3;
    for (int i = 0; i < languages; i++)
    {
        const char* code = MenuLocale::Language(i).code;
        useLanguage(code);
        checkEq(MenuLocale::ActiveCode(), code, "language could not be made active");

        size_t count = 0, kept = 0, dropped = 0;
        const MenuLocale::Tables::Entry* entries = MenuLocale::Tables::Find(code, count);
        for (size_t e = 0; e < count; e++)
        {
            kept += strcmp(entries[e].key, entries[e].text) == 0 ? 1 : 0;
            if (!MenuLocale::TranslationIsSafe(entries[e].key, entries[e].text))
            {
                std::printf("FAIL: %s: unsafe entry [%s]\n", code, entries[e].key);
                dropped++;
            }
            checkEq(MenuLocale::Text(entries[e].key), entries[e].text, "table not in effect");
        }
        failures += (int) dropped;

        testIds(code, plain, withId);
        const size_t missing = testGlyphs(code);
        std::printf("%-7s %-6zu %-6zu %s\n", code, count, kept, missing == 0 ? "ok" : "MISSING");

        checkEq(MenuLocale::Text("no such text##x"), "no such text##x", "unknown text falls through");

        // A format with ##: one %s and one %d stay, nothing repeats them
        const std::string fsr = MenuLocale::Format("FSR %s##%d");
        check(fsr.ends_with("##%d") && fsr.find("###") == fsr.npos && fsr.find("%s") == fsr.rfind("%s"),
              "format keeps its own ## suffix");

        if (!folder.empty() && frames)
            renderPicture(code, folder);
    }

    // The cost of one frame's worth of text.
    useLanguage(MenuLocale::Language(MenuLocale::LanguageCount() - 1).code);
    const auto start = std::chrono::steady_clock::now();
    size_t sink = 0;
    const int calls = 1000000;
    for (int i = 0; i < calls; i++)
        sink += (size_t) MenuLocale::Label(MenuLocale::Tables::Key(i % keys));
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("Label: %.0f ns a call (%zu), so 500 widgets take %.3f ms a frame\n", ns / calls, sink & 1, ns / calls * 500 / 1e6);

    // After frames with no renderer backend to own the textures, teardown crashes, in English too. With arguments
    // "<folder> <languages> noframes" it runs, and that covers the merged fonts.
    if (!frames)
        ImGui::DestroyContext();
    std::printf(failures == 0 ? "ALL OK\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
