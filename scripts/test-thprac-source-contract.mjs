import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const project = path.resolve(here, '..');
const workspace = path.resolve(project, '..');

function read(rel) {
    return fs.readFileSync(path.join(workspace, rel), 'utf8').replaceAll('\r\n', '\n');
}

const upstream = read('thprac-reallyportable/thprac/src/thprac/thprac_th07.cpp');
const catalog = read('thprac-reallyportable/portable/generated/section_catalog.hpp');
const portable = read('th07-eagler/src/PracticeRuntime.cpp');
const portableAscii = read('th07-eagler/src/AsciiManager.cpp');
const portableMain = read('th07-eagler/src/main.cpp');
const portableWindow = read('th07-eagler/src/GameWindow.cpp');
const portableGles = read('th07-eagler/src/graphics/Gles.cpp');
const portableCmake = read('th07-eagler/CMakeLists.txt');
const portableReplay = read('th07-eagler/src/ReplayManager.cpp');
const portableImGui = read('th07-eagler/src/ThpracImGui.cpp');
const portableSession = read('thprac-reallyportable/portable/src/session.cpp');

// TH07 has its own menu/runtime lifetime. State(1) clears live thPracParam but
// does not recreate THGuiPrac's widget members; State(3) is the only menu
// commit path. Unlike TH06 v2.3.0.3, TH07 defines no THPauseMenu/State(5).
for (const anchor of [
    'class THGuiPrac : public Gui::GameGuiWnd',
    'case 1:',
    'THReset();',
    'case 3:',
    'thPracParam.mode = *mMode;',
    'thPracParam.stage = *mStage;',
    'thPracParam.section = CalcSection();',
    'thPracParam.phase = SpellPhase() ? *mPhase : 0;',
    'thPracParam.point_total = *mPointTotal;',
    'thPracParam.point_stage = *mPointStage;',
    'thPracParam.cherryMax = *mCherryMax;',
    'thPracParam.rankLock = *mRankLock;',
]) {
    if (!upstream.includes(anchor))
        throw new Error(`Upstream TH07 Practice state contract missing: ${anchor}`);
}
if (upstream.includes('class THPauseMenu') || upstream.includes('THPauseMenu::singleton()'))
    throw new Error('Upstream TH07 unexpectedly acquired a TH06-style THPauseMenu');

// TH07 stage list is 1..6 + Extra + Phantasm, with stage-specific chapter
// cardinalities. Keep these independent from TH06.
for (const anchor of [
    'Gui::GuiCombo mStage { TH_STAGE, TH07_STAGE_SELECT };',
    '{ 2, 1 },',
    '{ 1, 1 },',
    '{ 4, 4 },',
    '{ 5, 3 }',
]) {
    if (!upstream.includes(anchor))
        throw new Error(`Upstream TH07 stage/chapter contract missing: ${anchor}`);
}

// Original TH07 chooses section IDs from th_sections_cba/cbt by stage+warp.
// mDiffculty is only the visible string-table selector; it is not an
// availability filter. This is especially important for Extra/Phantasm,
// whose generated rows carry only the 0x10 source mask.
for (const anchor of [
    'th_sections_cba[*mStage][*mWarp - 2]',
    'th_sections_cbt[*mStage][*mWarp - 4]',
    'th_sections_str[::THPrac::Gui::LocaleGet()][mDiffculty]',
]) {
    if (!upstream.includes(anchor))
        throw new Error(`Upstream TH07 section-selection contract missing: ${anchor}`);
}

const start = catalog.indexOf('inline constexpr SectionLabel th07SectionLabels[] = {');
const end = catalog.indexOf('inline constexpr std::size_t th07SectionLabelCount', start);
if (start < 0 || end < 0)
    throw new Error('Generated TH07 section catalogue boundary is missing');
const rows = [...catalog.slice(start, end).matchAll(/^\s*\{(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(0x[0-9a-f]+),/gmi)]
    .map(match => ({
        id: Number(match[1]), patchId: Number(match[2]), stage: Number(match[3]),
        bgm: Number(match[4]), spell: Number(match[5]), mask: Number.parseInt(match[6], 16),
    }));
if (rows.length === 0 || rows.some(row => row.id !== row.patchId))
    throw new Error('TH07 generated catalogue unexpectedly changed id/patchId identity');
const unique = values => [...new Set(values)];
const extraIds = unique(rows.filter(row => row.stage === 6).map(row => row.patchId));
const phantasmIds = unique(rows.filter(row => row.stage === 7).map(row => row.patchId));
if (extraIds.length !== 21 || extraIds[0] !== 61 || extraIds.at(-1) !== 81 ||
    phantasmIds.length !== 22 || phantasmIds[0] !== 82 || phantasmIds.at(-1) !== 103 ||
    rows.filter(row => row.stage >= 6).some(row => row.mask !== 0x10)) {
    throw new Error('TH07 Extra/Phantasm generated catalogue cardinality/mask drifted');
}

// Portable target contracts. These intentionally fail against the historical
// half-port until the corresponding audit items are fixed.
for (const anchor of [
    'static Config g_MenuConfig',
    'BuildSectionMatchesFor(',
    'keep the first row so the section ID remains available',
    'g_Config = g_MenuConfig;',
    'MenuVisualState::Opening',
    'g_MenuCloseStep = 0.8f;',
    'RequestMenuClose(MenuResult::Cancelled, false)',
    'RequestMenuClose(MenuResult::Accepted, true)',
    'ImGui::SetNextWindowSize(chinese ? ImVec2(330.0f, 390.0f) : ImVec2(400.0f, 390.0f)',
    'locale == ThpracImGui::Locale::EnUS ? -80.0f',
    'locale == ThpracImGui::Locale::JaJP ? -65.0f : -60.0f',
    'ThpracImGui::TextId::PointTotal',
    'ThpracImGui::TextId::PointStage',
    'ThpracImGui::TextId::Cherry',
    'ThpracImGui::TextId::CherryMax',
    'ThpracImGui::TextId::CherryPlus',
    'ThpracImGui::TextId::SpellBonus',
]) {
    if (!portable.includes(anchor))
        throw new Error(`Portable TH07 audited contract missing: ${anchor}`);
}
if (portable.includes('entry.stage != g_Config.stage || !(entry.difficultyMask & difficultyBit)') ||
    portable.includes('SDL_IOFromFile("thprac-session.json", "rb")') ||
    portable.includes('LoadDesktopSession();')) {
    throw new Error('Portable TH07 still contains launcher/session-era desktop gating or difficulty-as-availability filtering');
}
if (!portable.includes('return g_GameManager.practice != 0;') ||
    portable.includes('return g_GameManager.practice != 0 && g_GameManager.replay == 0;'))
    throw new Error('Portable TH07 Practice entry must follow upstream th07_prac_menu_1 and ignore stale replay ownership');

// Chapter is a normal visible SliderInt. The encoded section is only allowed
// to seed the widget on an explicit restore/sync path; live mouse/left/right
// edits must flow g_MenuChapter -> section without being rolled back.
const chapterStateStart = portable.indexOf('if (g_Config.warp == 1)');
const chapterStateEnd = portable.indexOf('\n    if (g_Config.warp == 6)', chapterStateStart);
const chapterState = portable.slice(chapterStateStart, chapterStateEnd);
if (!chapterState.includes('else if (syncFromConfig && g_Config.section >= 10000') ||
    !portable.includes('GuiSliderInt(sectionLabel, &g_MenuChapter, 1, limit, chapterStep, format)') ||
    !portable.includes('CurrentSection(0, false);') ||
    !portable.includes('bool changed = ImGui::SliderInt(label, value, minimum, maximum, format);') ||
    portable.includes('relativeMouse')) {
    throw new Error('Portable TH07 Chapter slider/input ownership regressed');
}

// THGuiPrac itself is a pinned Dear ImGui 1.82 same-process window, not the
// historical ASCII placeholder.  The bridge samples game directions on the
// fixed tick, feeds SDL mouse events, draws after the game into the 640x480
// game FBO, and restores GL state before normal presentation.
for (const [source, anchor, label] of [
    [portableCmake, 'src/ThpracImGui.cpp', 'TH07 CMake thprac ImGui source'],
    [portableCmake, '${THPRAC_IMGUI_ROOT}/imgui.cpp', 'TH07 pinned ImGui core'],
    [portableCmake, 'IMGUI_DISABLE_WIN32_FUNCTIONS', 'TH07 no Win32 ImGui backend'],
    [portableCmake, 'Freetype::Freetype', 'TH07 pinned ImGui FreeType atlas'],
    [portableMain, 'ThpracImGui::ProcessEvent(*event);', 'TH07 SDL event bridge'],
    [portableMain, '#if defined(TH_DEV_TOOLS) && !defined(TH_ENABLE_THPRAC)', 'TH07 F5 thprac ownership'],
    [portableWindow, 'ThpracImGui::SetGameInput(g_CurFrameRawInput, updated);', 'TH07 fixed-tick game-input bridge'],
    [portableWindow, 'ThpracImGui::BeginFrame(static_cast<f32>(targetDt));', 'TH07 fixed-tick ImGui frame'],
    [portableWindow, 'RenderImGui(ThpracImGui::GetDrawData())', 'TH07 same-FBO ImGui render'],
    [portableGles, 'glBindFramebuffer(GL_FRAMEBUFFER, this->fbo);', 'TH07 ImGui game-FBO target'],
    [portableGles, 'glViewport(0, 0, 640, 480);', 'TH07 ImGui logical surface'],
    [portableGles, 'this->stateCache.Invalidate();', 'TH07 ImGui GL-state restoration boundary'],
    [portableImGui, 'GetGlyphRangesChineseFull()', 'TH07 Full CJK trainer atlas'],
    [portableImGui, 'g_LastFrameRawInput', 'TH07 Gen1 input previous sample'],
    [portableImGui, 'g_IsEighthFrameOfHeldInput', 'TH07 Gen1 8-frame repeat'],
]) {
    if (!source.includes(anchor))
        throw new Error(`${label} missing: ${anchor}`);
}
if (portable.includes('thprac - Advanced Practice') ||
    portable.includes('ScreenEffect::DrawSquare(&panel, 0xff181820)'))
    throw new Error('Portable TH07 still contains the historical ASCII Practice placeholder');

// Exact TH07 final-spell phase sets. Extra is Normal/Full/Rage while
// Phantasm is Normal/Rage; treating both as a boolean phase loses upstream
// functionality.
for (const anchor of [
    'if (section == 81)',
    'return 3;',
    'if (section == 102)',
    'return 2;',
    'phaseRagefulSelector[] = {1, 2, 3, 0}',
    'ThpracImGui::TextId::Full',
    'phaseSimpleSelector[] = {1, 2, 0}',
]) {
    if (!portable.includes(anchor))
        throw new Error(`Portable TH07 Extra/Phantasm phase contract missing: ${anchor}`);
}

// TH07 replay metadata is embedded in the actual T7RP replay.  The previous
// .thprac.json sidecar was an undeployed portable invention and must not be
// retained as a compatibility branch.  Upstream's multichar 'CARP' lands as
// PRAC bytes on little-endian x86.
for (const anchor of [
    'std::memcmp(bytes.data(), "T7RP", 4)',
    'std::memcmp(bytes.data() + bytes.size() - 4, "PRAC", 4)',
    'json.find("\\\"version\\\":")',
    '"{\\\"version\\\":\\\"2.3.0.3\\\",\\\"game\\\":\\\"th07\\\",\\\"mode\\\":%d,\\\"stage\\\":%d"',
    'std::vector<u8> checksumBytes(bytes.begin() + 13, bytes.end());',
    'for (std::size_t i = 0; i < bytes.size() - 16; ++i)',
    'checksumBytes[3 + i] = static_cast<u8>(checksumBytes[3 + i] - key);',
    'key = static_cast<u8>(key + 7);',
    'u32 checksum = 0x3f000318;',
    'WriteLe32(bytes.data() + 8, checksum);',
]) {
    if (!portable.includes(anchor))
        throw new Error(`Portable TH07 upstream replay contract missing: ${anchor}`);
}
if (portable.includes('.thprac.json') || portable.includes('"CARP"'))
    throw new Error('Portable TH07 still contains undeployed sidecar/CARP replay compatibility');
if (!portableReplay.includes('PracticeRuntime::SaveReplayMetadata('))
    throw new Error('TH07 ReplayManager no longer appends upstream thprac metadata after saving');
if (!portableSession.includes('const bool upstreamSchema = json.find("\\\"version\\\":") != std::string::npos;') ||
    !portableSession.includes('(!portableSchema && !upstreamSchema)'))
    throw new Error('TH07 adapter no longer accepts upstream version/game PRAC metadata JSON');

// TH07 has no TH06 custom Pause/Restart/State(5) parameter lifecycle.
for (const forbidden of [
    'g_PreserveConfigOnRestart',
    'DebugRestartPreservesConfig',
    '--thprac-restart-selftest',
    'class THPauseMenu',
]) {
    if (portable.includes(forbidden) || portableMain.includes(forbidden))
        throw new Error(`Portable TH07 retained TH06-only restart/pause concept: ${forbidden}`);
}

// TH07 upstream does not replace the game Pause UI. The portable adapter must
// therefore not suppress AsciiManager's vanilla pause draw/update path.
if (portableAscii.includes('PracticeRuntime::DrawPauseMenuPanel()') ||
    portableAscii.includes('if (!PracticeRuntime::UpdatePauseMenu())')) {
    throw new Error('Portable TH07 still installs the non-upstream TH06-style Pause overlay');
}

console.log(
    'TH07 thprac source contract PASS: TH07-specific Stage1..6/Extra/Phantasm menu; ' +
    'section availability follows cba/cbt while difficulty selects duplicate labels only; ' +
    'persistent THGuiPrac widget state is separate from State(3) live params; real pinned ImGui/GLES bridge; ' +
    'Extra=Normal/Full/Rage and Phantasm=Normal/Rage; in-file T7RP/PRAC replay metadata; ' +
    'desktop entry is stale-replay-safe real Practice flow; vanilla TH07 owns Pause'
);
