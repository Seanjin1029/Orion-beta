#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// ---- Light LookAndFeel -------------------------------------------------------
struct LightLookAndFeel : public juce::LookAndFeel_V4
{
    LightLookAndFeel()
    {
        setColour(juce::TextEditor::backgroundColourId,      juce::Colours::white);
        setColour(juce::TextEditor::textColourId,            juce::Colour(0xFF1A1A1A));
        setColour(juce::TextEditor::highlightColourId,       juce::Colour(0x33000000));
        setColour(juce::TextEditor::highlightedTextColourId, juce::Colour(0xFF1A1A1A));
        setColour(juce::TextEditor::outlineColourId,         juce::Colours::transparentBlack);
        setColour(juce::TextEditor::focusedOutlineColourId,  juce::Colours::transparentBlack);
        setColour(juce::CaretComponent::caretColourId,       juce::Colour(0xFF1A1A1A));
        setColour(juce::ScrollBar::thumbColourId,            juce::Colour(0x55000000));
        setColour(juce::ScrollBar::backgroundColourId,       juce::Colours::transparentBlack);
    }

    void drawTextEditorOutline(juce::Graphics&, int, int, juce::TextEditor&) override {}

    void drawButtonBackground(juce::Graphics& g, juce::Button& b,
                              const juce::Colour&, bool over, bool) override
    {
        // Plus button (empty label) — circle is drawn by paintOverChildren.
        if (b.getButtonText().isEmpty()) return;
        g.setColour(juce::Colour(over ? 0xFFE0E0E0 : 0xFFF1F1F1));
        g.fillRoundedRectangle(b.getLocalBounds().toFloat(), 5.f);
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        if (b.getButtonText().isEmpty()) return;
        g.setColour(juce::Colour(0xFF1A1A1A));
        g.setFont(juce::Font("Inter", 11.f, juce::Font::plain));
        g.drawText(b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
    }
};

// ---- Layer list component ----------------------------------------------------
class LayerListComponent : public juce::Component
{
public:
    struct Row
    {
        juce::String name;
        AbletonAIPluginAudioProcessor::LayerInfo::Status status;
    };

    std::function<void(int)> onLayerClicked;

    void setRows(const std::vector<Row>& rows, int activeIndex)
    {
        rows_        = rows;
        activeIndex_ = activeIndex;
        repaint();
    }

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

    static constexpr int kRowH = 38;

private:
    std::vector<Row> rows_;
    int              activeIndex_ { -1 };
};

// ---- Editor ------------------------------------------------------------------
class AbletonAIPluginAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit AbletonAIPluginAudioProcessorEditor(AbletonAIPluginAudioProcessor&);
    ~AbletonAIPluginAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;

private:
    void onSendClicked();
    void onCompileClicked();
    void updateFromProcessor();
    void refreshLayerList();

    AbletonAIPluginAudioProcessor& processor;
    LightLookAndFeel               lightFeel;
    juce::ApplicationProperties    appProps;

    // Left panel
    juce::Label      statusLabel;
    juce::Label      fallbackBadge;
    juce::TextEditor codeViewer;
    juce::TextButton compileButton;

    // Right panel
    juce::Label      keyPrefixLabel;
    juce::TextEditor keyEditor;
    LayerListComponent layerList;
    juce::TextEditor promptEditor;
    juce::TextButton plusButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AbletonAIPluginAudioProcessorEditor)
};
