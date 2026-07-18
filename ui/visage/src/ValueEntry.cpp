#include "factory_ui_visage/ValueEntry.h"
#include "factory_ui_visage/Fonts.h"

#include <utility>

namespace factory_ui_visage
{
    namespace { constexpr float kRounding = 6.0f; }

    ValueEntry::ValueEntry (const Theme& theme)
        : visage::TextEditor ("value-entry"), theme_ (theme)
    {
        const Palette& p = theme_.palette;
        // The JUCE Label editor colours: warm-white card, dark warm text, coral
        // caret / selection (0.35) / outline. Set on a private palette so the entry
        // themes itself regardless of the surrounding tree.
        palette_.setColor (TextEditorBackground,  visage::Color (p.panel));
        palette_.setColor (TextEditorText,        visage::Color (p.text));
        palette_.setColor (TextEditorDefaultText, visage::Color (p.textSecondary));
        palette_.setColor (TextEditorCaret,       visage::Color (p.accent));
        palette_.setColor (TextEditorSelection,   visage::Color (p.accent).withAlpha (0.35f));
        palette_.setColor (TextEditorBorder,      visage::Color (p.accent));
        setPalette (&palette_);

        setNumberEntry();             // single-line + select-on-focus + centred
        setBackgroundRounding (kRounding);
        setMargin (6.0f, 0.0f);
        setDefaultText ("");

        onEnterKey().add  ([this] { doCommit(); });   // Enter commits
        onEscapeKey().add ([this] { cancelEntry(); }); // Esc cancels
        setVisible (false);
    }

    void ValueEntry::resized()
    {
        visage::TextEditor::resized(); // base re-reads paletteValue(TextEditorRounding)…
        setBackgroundRounding (kRounding); // …so re-assert our warm rounded card
    }

    void ValueEntry::open (float x, float y, float w, float h, const std::string& prefill,
                           float fontPx, std::function<void (const std::string&)> commit)
    {
        prefill_ = prefill;
        commit_  = std::move (commit);
        setFont (boldFont (fontPx));
        setBounds (x, y, w, h);
        setText (prefill);
        open_ = true;
        finishing_ = false;
        setVisible (true); // added last in the editor -> renders above every sibling
        requestKeyboardFocus(); // -> focusChanged(true) -> selectAll() (select-on-focus)
        selectAll();
        redraw();
    }

    void ValueEntry::doCommit()
    {
        if (! open_ || finishing_) return;
        finishing_ = true;
        const std::string t = text().toUtf8();
        const bool changed = (t != prefill_);   // Label dedup: unchanged == cancel
        auto commit = commit_;
        finish();                                // hide first (open_ = false blocks re-entry)
        if (changed && commit) commit (t);
    }

    void ValueEntry::cancelEntry()
    {
        if (! open_ || finishing_) return;
        finishing_ = true;
        finish();
    }

    void ValueEntry::finish()
    {
        open_ = false;
        commit_ = nullptr;
        setVisible (false); // may re-enter focusChanged(false) — guarded by open_ == false
        finishing_ = false;
        redraw();
    }

    void ValueEntry::focusChanged (bool is_focused, bool was_clicked)
    {
        visage::TextEditor::focusChanged (is_focused, was_clicked); // keep select-on-focus
        if (! is_focused && open_ && ! finishing_)
            doCommit(); // focus-loss COMMITS (lossOfFocusDiscardsChanges = false)
    }
}
