#include "PluginEditor.h"

// ---- Layout (Figma 1109×623, Frame 13 = content area) ----------------------
static constexpr int kW        = 1109;
static constexpr int kH        = 623;
static constexpr int kDivX     = 718;   // divider absolute x (1 px wide line)
static constexpr int kLeftX    = 10;    // left panel outer x
static constexpr int kLeftW    = 692;   // left panel width
static constexpr int kRightX   = 736;   // right panel outer x
static constexpr int kRightW   = 363;   // right panel width
static constexpr int kPad      = 10;    // outer / gap padding
static constexpr int kIPad     = 5;     // input-bar inner padding
static constexpr int kKeyH     = 25;    // key row height   (Figma: 25)
static constexpr int kBtnSize  = 28;    // plus button size (24 icon + 2+2 px border)
static constexpr int kInputH   = 38;    // input bar height (Figma: 38)
// Absolute y of the input bar.  Frame 3 (right panel) starts at y=kPad in the
// content area and has its own kPad bottom margin, so two kPad levels apply.
static constexpr int kInputY   = kH - 2 * kPad - kInputH;  // = 565

// ---- Colours ----------------------------------------------------------------
static const juce::Colour kBg        = juce::Colours::white;
static const juce::Colour kDivider   = juce::Colour(0xFFA6A6A6);
static const juce::Colour kDim       = juce::Colour(0xFFA6A6A6);
static const juce::Colour kText      = juce::Colours::black;
static const juce::Colour kRowBg     = juce::Colour(0xFFF1F1F1);
static const juce::Colour kInput     = juce::Colour(0xFFFFF1D5);
static const juce::Colour kDotYellow = juce::Colour(0xFFE6A800);
static const juce::Colour kDotGreen  = juce::Colour(0xFF34A853);

// ---- Fonts ------------------------------------------------------------------
static juce::Font interFont(float sz = 12.f, bool bold = false)
{
    return juce::Font("Inter", sz, bold ? juce::Font::bold : juce::Font::plain);
}
static juce::Font monoFont(float sz = 11.5f)
{
    return juce::Font(juce::Font::getDefaultMonospacedFontName(), sz, juce::Font::plain);
}

// ---- LayerListComponent -----------------------------------------------------
// Row layout (per Figma): height=35, radius=9, bg=#F1F1F1, gap between rows=10
static constexpr int kLayerRowH   = 35;
static constexpr int kLayerRowGap = 10;

void LayerListComponent::paint(juce::Graphics& g)
{
    for (int i = 0; i < (int)rows_.size(); ++i)
    {
        const int y         = i * (kLayerRowH + kLayerRowGap);
        const bool isActive = (i == activeIndex_);
        const auto& row     = rows_[i];

        // Row background
        g.setColour(kRowBg);
        g.fillRoundedRectangle(0.f, (float)y, (float)getWidth(), (float)kLayerRowH, 9.f);

        // Layer name — black when active, dimmed otherwise
        g.setFont(interFont(12.f));
        g.setColour(isActive ? kText : kDim);
        g.drawText(row.name, kIPad + kIPad, y, getWidth() - 24, kLayerRowH,
                   juce::Justification::centredLeft);

        // Status dot — shown only for active layer
        // Position matches Figma: Frame 21 (17×17) at x=321 within 343px row,
        // Ellipse (7×7) at x=5,y=5 inside that → centre ≈ (width-13.5, rowMid)
        if (isActive)
        {
            const bool compiling =
                (row.status == AbletonAIPluginAudioProcessor::LayerInfo::Status::Compiling);
            g.setColour(compiling ? kDotYellow : kDotGreen);
            const float dotD  = 7.f;
            const float dotCX = (float)getWidth() - 13.5f;
            const float dotCY = (float)y + kLayerRowH * 0.5f;
            g.fillEllipse(dotCX - dotD * 0.5f, dotCY - dotD * 0.5f, dotD, dotD);
        }
    }
}

void LayerListComponent::mouseDown(const juce::MouseEvent& e)
{
    const int idx = e.y / (kLayerRowH + kLayerRowGap);
    if (idx >= 0 && idx < (int)rows_.size() && onLayerClicked)
        onLayerClicked(idx);
}

// ---- Constructor ------------------------------------------------------------
AbletonAIPluginAudioProcessorEditor::AbletonAIPluginAudioProcessorEditor(
        AbletonAIPluginAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setLookAndFeel(&lightFeel);
    setSize(kW, kH);
    setResizable(false, false);

    // ---- Left: status label ------------------------------------------------
    statusLabel.setFont(interFont(12.f));
    statusLabel.setColour(juce::Label::textColourId, kDim);
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setText("Nothing Yet..", juce::dontSendNotification);
    addAndMakeVisible(statusLabel);

    fallbackBadge.setFont(interFont(10.f, true));
    fallbackBadge.setColour(juce::Label::backgroundColourId, juce::Colour(0xFFFF3B30));
    fallbackBadge.setColour(juce::Label::textColourId, juce::Colours::white);
    fallbackBadge.setJustificationType(juce::Justification::centred);
    fallbackBadge.setText("C++ fallback", juce::dontSendNotification);
    fallbackBadge.setVisible(false);
    addAndMakeVisible(fallbackBadge);

    // ---- Left: code viewer (editable) --------------------------------------
    codeViewer.setMultiLine(true);
    codeViewer.setReadOnly(false);
    codeViewer.setScrollbarsShown(true);
    codeViewer.setFont(monoFont());
    codeViewer.setColour(juce::TextEditor::backgroundColourId, juce::Colours::white);
    codeViewer.setColour(juce::TextEditor::textColourId, kText);
    codeViewer.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    codeViewer.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(codeViewer);

    // ---- Left: compile button ----------------------------------------------
    compileButton.setButtonText("Compile");
    compileButton.onClick = [this] { onCompileClicked(); };
    addAndMakeVisible(compileButton);

    // ---- Right: API key row ------------------------------------------------
    keyPrefixLabel.setText("Key:", juce::dontSendNotification);
    keyPrefixLabel.setFont(interFont(12.f));
    keyPrefixLabel.setColour(juce::Label::textColourId, kText);
    keyPrefixLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(keyPrefixLabel);

    keyEditor.setPasswordCharacter(0x2022);
    keyEditor.setFont(interFont(12.f));
    keyEditor.setJustification(juce::Justification::centredLeft);
    keyEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    keyEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    keyEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    keyEditor.setColour(juce::TextEditor::textColourId, kText);
    addAndMakeVisible(keyEditor);

    // ---- Right: layer list -------------------------------------------------
    layerList.onLayerClicked = [this](int index) {
        processor.activateLayer(index);
    };
    addAndMakeVisible(layerList);

    // ---- Right: prompt input -----------------------------------------------
    // IMPORTANT: background must be kInput (not transparent) so typed text is
    // visible.  The pill is drawn in paint() BEHIND children — not in
    // paintOverChildren() which would paint OVER the text.
    promptEditor.setMultiLine(false);
    promptEditor.setReturnKeyStartsNewLine(false);
    promptEditor.setFont(interFont(12.f));
    promptEditor.setColour(juce::TextEditor::backgroundColourId, kInput);
    promptEditor.setColour(juce::TextEditor::textColourId, kText);
    promptEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    promptEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    promptEditor.setTextToShowWhenEmpty("Type Anything...", kDim);
    promptEditor.onReturnKey = [this] { onSendClicked(); };
    addAndMakeVisible(promptEditor);

    // ---- Right: + button (decorative; circle drawn in paintOverChildren) ---
    plusButton.setButtonText("");
    addAndMakeVisible(plusButton);

    // ---- Restore API key ---------------------------------------------------
    juce::PropertiesFile::Options opts;
    opts.applicationName     = "AbletonAIPlugin";
    opts.filenameSuffix      = "settings";
    opts.osxLibrarySubFolder = "Application Support";
    appProps.setStorageParameters(opts);
    if (auto* s = appProps.getUserSettings())
        keyEditor.setText(s->getValue("apiKey", ""), false);

    // ---- Restore state from processor --------------------------------------
    {
        const auto code   = processor.getLastFaustCode();
        const auto prompt = processor.getLastPrompt();
        if (code.isNotEmpty())
        {
            codeViewer.setText(code, false);
            codeViewer.moveCaretToTop(false);
        }
        if (prompt.isNotEmpty())
            promptEditor.setText(prompt, false);
    }
    refreshLayerList();

    processor.onStatusChanged = [this] { updateFromProcessor(); };
}

AbletonAIPluginAudioProcessorEditor::~AbletonAIPluginAudioProcessorEditor()
{
    processor.onStatusChanged = nullptr;
    setLookAndFeel(nullptr);
}

// ---- Paint ------------------------------------------------------------------
// Drawn BEFORE children — pill here is the background the prompt editor sits on.
void AbletonAIPluginAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBg);

    // Input bar pill — drawn here (behind children) so text inside promptEditor
    // is never obscured.
    const juce::Rectangle<float> pill(
        (float)(kRightX + kPad), (float)kInputY,
        (float)(kRightW - 2 * kPad), (float)kInputH);
    g.setColour(kInput);
    g.fillRoundedRectangle(pill, 24.f);
}

// ---- Resized ----------------------------------------------------------------
void AbletonAIPluginAudioProcessorEditor::resized()
{
    // Figma uses two levels of padding: outer panel gap (kPad) + inner p-[10px].
    // Content inside left/right panels starts at x = panelX + kPad,
    // y = kPad (outer) + kPad (inner) = 20.

    const int innerY0 = kPad + kPad;  // 20 — top of right/left panel content

    // ---- Left panel --------------------------------------------------------
    // Status label + Compile button in the header row
    const int statusH     = 20;
    const int compileBtnW = 65;
    statusLabel  .setBounds(kLeftX + kPad, innerY0, kLeftW - 3 * kPad - compileBtnW, statusH);
    fallbackBadge.setBounds(kLeftX + kPad, innerY0, kLeftW - 3 * kPad - compileBtnW, statusH);
    compileButton.setBounds(kLeftX + kLeftW - kPad - compileBtnW, innerY0, compileBtnW, statusH);

    // Code viewer: after header + 10px gap
    const int codeY = innerY0 + statusH + kPad;  // 50
    const int codeH = (kH - kPad) - kPad - codeY;
    codeViewer.setBounds(kLeftX + kPad, codeY, kLeftW - 2 * kPad, codeH);

    // ---- Right panel -------------------------------------------------------
    // Key row: Figma Frame 4 at Frame3-local (10,10), absolute (746,20), h=25
    keyPrefixLabel.setBounds(kRightX + kPad, innerY0, 30, kKeyH);
    keyEditor     .setBounds(kRightX + kPad + 32, innerY0,
                             kRightW - 2 * kPad - 32, kKeyH);

    // Layer list: starts at key_bottom + gap = 20+25+10 = 55
    const int layerY = innerY0 + kKeyH + kPad;       // 55
    const int layerH = kInputY - layerY - kPad;      // 500
    layerList.setBounds(kRightX + kPad, layerY, kRightW - 2 * kPad, layerH);

    // Input bar contents sit inside the pill drawn in paint().
    // Pill: x=746, y=565, w=343, h=38.  Inner padding kIPad=5.
    const int pillX = kRightX + kPad;                // 746
    const int btnY  = kInputY + (kInputH - kBtnSize) / 2;  // vertically centred
    plusButton.setBounds(pillX + kIPad, btnY, kBtnSize, kBtnSize);

    const int promptX = pillX + kIPad + kBtnSize + kPad;   // 789
    const int promptW = (pillX + kRightW - 2 * kPad) - promptX - kIPad;  // ≈295
    const int promptY = kInputY + kIPad;
    const int promptH = kInputH - 2 * kIPad;
    promptEditor.setBounds(promptX, promptY, promptW, promptH);
}

// ---- paintOverChildren — draws the + circle outline only --------------------
void AbletonAIPluginAudioProcessorEditor::paintOverChildren(juce::Graphics& g)
{
    // Circle outline around plusButton (button itself has no background fill)
    g.setColour(kText);
    g.drawEllipse(plusButton.getBounds().toFloat().reduced(0.5f), 1.f);
}

// ---- Actions ----------------------------------------------------------------
void AbletonAIPluginAudioProcessorEditor::onSendClicked()
{
    const auto prompt = promptEditor.getText().trim();
    const auto apiKey = keyEditor.getText().trim();

    if (prompt.isEmpty() || apiKey.isEmpty()) return;

    if (auto* s = appProps.getUserSettings())
    {
        s->setValue("apiKey", apiKey);
        s->saveIfNeeded();
    }
    processor.setLastPrompt(prompt);
    promptEditor.clear();
    processor.startGeneration(prompt, apiKey);
}

void AbletonAIPluginAudioProcessorEditor::onCompileClicked()
{
    const auto code = codeViewer.getText().trim();
    if (code.isEmpty()) return;
    processor.compileCode(code);
}

void AbletonAIPluginAudioProcessorEditor::refreshLayerList()
{
    const int count  = processor.getLayerCount();
    const int active = processor.getActiveLayerIndex();

    std::vector<LayerListComponent::Row> rows;
    rows.reserve((size_t)count);
    for (int i = 0; i < count; ++i)
    {
        const auto info = processor.getLayer(i);
        rows.push_back({ "Layer " + juce::String(i + 1) + ": " + info.name, info.status });
    }
    layerList.setRows(rows, active);
}

void AbletonAIPluginAudioProcessorEditor::updateFromProcessor()
{
    using S = AbletonAIPluginAudioProcessor::Status;
    const auto status = processor.getStatus();

    if (status == S::Generating)
    {
        statusLabel.setText("compiling...", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, kDim);
        fallbackBadge.setVisible(false);
    }
    else if (status == S::Error)
    {
        statusLabel.setText("error", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFF3B30));
        fallbackBadge.setVisible(false);
    }
    else
    {
        const int activeIdx = processor.getActiveLayerIndex();
        const bool isFallback = (activeIdx >= 0) &&
            (processor.getLayer(activeIdx).status ==
             AbletonAIPluginAudioProcessor::LayerInfo::Status::Fallback);

        if (isFallback)
        {
            statusLabel.setText("C++ fallback", juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, kDim);
            fallbackBadge.setVisible(false);
        }
        else if (activeIdx >= 0)
        {
            statusLabel.setText("compiled!", juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, kDotGreen);
            fallbackBadge.setVisible(false);
        }
        else
        {
            statusLabel.setText("Nothing Yet..", juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, kDim);
            fallbackBadge.setVisible(false);
        }

        const auto code = processor.getLastFaustCode();
        if (code.isNotEmpty())
        {
            codeViewer.setText(code, false);
            codeViewer.moveCaretToTop(false);
        }
    }

    refreshLayerList();
}
