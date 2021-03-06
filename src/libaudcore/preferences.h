/*
 * preferences.h
 * Copyright 2007-2012 Tomasz Moń, William Pitcock, and John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#ifndef LIBAUDCORE_PREFERENCES_H
#define LIBAUDCORE_PREFERENCES_H

#include <libaudcore/objects.h>

struct PreferencesWidget;

struct ComboBoxElements {
    const void * value;
    const char * label;
};

struct WidgetVRadio {
    int value;
};

struct WidgetVSpin {
    double min, max, step;
    const char * right_label; /* text right to widget */
};

struct WidgetVTable {
    ArrayRef<const PreferencesWidget> widgets;
};

struct WidgetVLabel {
    const char * stock_id;
    bool single_line; /* false to enable line wrap */
};

struct WidgetVFonts {
    const char * title;
};

struct WidgetVEntry {
    bool password;
};

struct WidgetVCombo {
    /* static init */
    ArrayRef<const ComboBoxElements> elems;

    /* runtime init */
    ArrayRef<const ComboBoxElements> (* fill) ();
};

struct WidgetVBox {
    ArrayRef<const PreferencesWidget> widgets;

    bool horizontal;  /* false gives vertical, true gives horizontal aligment of child widgets */
    bool frame;       /* whether to draw frame around box */
};

struct NotebookTab {
    const char * name;
    ArrayRef<const PreferencesWidget> widgets;
};

struct WidgetVNotebook {
    ArrayRef<const NotebookTab> tabs;
};

struct WidgetVSeparator {
    bool horizontal;  /* false gives vertical, true gives horizontal separator */
};

union WidgetVariant {
    struct WidgetVRadio radio_btn;
    struct WidgetVSpin spin_btn;
    struct WidgetVTable table;
    struct WidgetVLabel label;
    struct WidgetVFonts font_btn;
    struct WidgetVEntry entry;
    struct WidgetVCombo combo;
    struct WidgetVBox box;
    struct WidgetVNotebook notebook;
    struct WidgetVSeparator separator;

    /* for WIDGET_CUSTOM */
    /* GtkWidget * (* populate) (void); */
    void * (* populate) (void);

    constexpr WidgetVariant (WidgetVRadio radio) : radio_btn (radio) {}
    constexpr WidgetVariant (WidgetVSpin spin) : spin_btn (spin) {}
    constexpr WidgetVariant (WidgetVTable table) : table (table) {}
    constexpr WidgetVariant (WidgetVLabel label) : label (label) {}
    constexpr WidgetVariant (WidgetVFonts fonts) : font_btn (fonts) {}
    constexpr WidgetVariant (WidgetVEntry entry) : entry (entry) {}
    constexpr WidgetVariant (WidgetVCombo combo) : combo (combo) {}
    constexpr WidgetVariant (WidgetVBox box) : box (box) {}
    constexpr WidgetVariant (WidgetVNotebook notebook) : notebook (notebook) {}
    constexpr WidgetVariant (WidgetVSeparator separator) : separator (separator) {}

    /* also serves as default constructor */
    constexpr WidgetVariant (void * (* populate) (void) = 0) : populate (populate) {}
};

struct WidgetConfig
{
    enum Type {
        None,
        Bool,
        Int,
        Float,
        String
    };

    Type type;
    void * value;
    const char * section, * name;
    void (* callback) (void);

    constexpr WidgetConfig () :
        type (None),
        value (nullptr),
        section (nullptr),
        name (nullptr),
        callback (nullptr) {}

    constexpr WidgetConfig (Type type, void * value, const char * section,
     const char * name, void (* callback) (void)) :
        type (type),
        value (value),
        section (section),
        name (name),
        callback (callback) {}
};

constexpr WidgetConfig WidgetBool (bool & value, void (* callback) (void) = nullptr)
    { return WidgetConfig (WidgetConfig::Bool, (void *) & value, 0, 0, callback); }
constexpr WidgetConfig WidgetInt (int & value, void (* callback) (void) = nullptr)
    { return WidgetConfig (WidgetConfig::Int, (void *) & value, 0, 0, callback); }
constexpr WidgetConfig WidgetFloat (double & value, void (* callback) (void) = nullptr)
    { return WidgetConfig (WidgetConfig::Float, (void *) & value, 0, 0, callback); }
constexpr WidgetConfig WidgetString (String & value, void (* callback) (void) = nullptr)
    { return WidgetConfig (WidgetConfig::String, (void *) & value, 0, 0, callback); }

constexpr WidgetConfig WidgetBool (const char * section, const char * name, void (* callback) (void) = nullptr)
    { return WidgetConfig (WidgetConfig::Bool, 0, section, name, callback); }
constexpr WidgetConfig WidgetInt (const char * section, const char * name, void (* callback) (void) = nullptr)
    { return WidgetConfig (WidgetConfig::Int, 0, section, name, callback); }
constexpr WidgetConfig WidgetFloat (const char * section, const char * name, void (* callback) (void) = nullptr)
    { return WidgetConfig (WidgetConfig::Float, 0, section, name, callback); }
constexpr WidgetConfig WidgetString (const char * section, const char * name, void (* callback) (void) = nullptr)
    { return WidgetConfig (WidgetConfig::String, 0, section, name, callback); }

struct PreferencesWidget
{
    enum Type {
        Label,
        CheckButton,
        RadioButton,
        SpinButton,
        Entry,
        ComboBox,
        FontButton,
        Box,
        Table,
        Notebook,
        Separator,
        Custom
    };

    Type type;
    const char * label;       /* widget title (for SPIN_BTN it's text left to widget) */
    const char * tooltip;     /* widget tooltip, can be nullptr */
    bool child;

    WidgetConfig cfg;
    WidgetVariant data;
};

enum WidgetIsChild {
    WIDGET_NOT_CHILD,
    WIDGET_CHILD
};

constexpr const PreferencesWidget WidgetLabel (const char * label,
 WidgetIsChild child = WIDGET_NOT_CHILD)
    { return {PreferencesWidget::Label, label, 0, (child == WIDGET_CHILD)}; }

constexpr const PreferencesWidget WidgetCheck (const char * label,
 WidgetConfig cfg, WidgetIsChild child = WIDGET_NOT_CHILD)
    { return {PreferencesWidget::CheckButton, label, 0,
       (child == WIDGET_CHILD), cfg}; }

constexpr const PreferencesWidget WidgetRadio (const char * label,
 WidgetConfig cfg, WidgetVRadio radio, WidgetIsChild child = WIDGET_NOT_CHILD)
    { return {PreferencesWidget::RadioButton, label, 0,
       (child == WIDGET_CHILD), cfg, radio}; }

constexpr const PreferencesWidget WidgetSpin (const char * label,
 WidgetConfig cfg, WidgetVSpin spin, WidgetIsChild child = WIDGET_NOT_CHILD)
    { return {PreferencesWidget::SpinButton, label, 0,
       (child == WIDGET_CHILD), cfg, spin}; }

constexpr const PreferencesWidget WidgetEntry (const char * label,
 WidgetConfig cfg, WidgetVEntry entry = WidgetVEntry(),
 WidgetIsChild child = WIDGET_NOT_CHILD)
    { return {PreferencesWidget::Entry, label, 0,
       (child == WIDGET_CHILD), cfg, entry}; }

constexpr const PreferencesWidget WidgetCombo (const char * label,
 WidgetConfig cfg, WidgetVCombo combo, WidgetIsChild child = WIDGET_NOT_CHILD)
    { return {PreferencesWidget::ComboBox, label, 0,
       (child == WIDGET_CHILD), cfg, combo}; }

constexpr const PreferencesWidget WidgetFonts (const char * label,
 WidgetConfig cfg, WidgetVFonts fonts, WidgetIsChild child = WIDGET_NOT_CHILD)
    { return {PreferencesWidget::FontButton, label, 0,
       (child == WIDGET_CHILD), cfg, fonts}; }

constexpr const PreferencesWidget WidgetBox (WidgetVBox box, WidgetIsChild child = WIDGET_NOT_CHILD)
    { return {PreferencesWidget::Box, 0, 0, (child == WIDGET_CHILD), WidgetConfig (), box}; }

constexpr const PreferencesWidget WidgetTable (WidgetVTable table,
 WidgetIsChild child = WIDGET_NOT_CHILD)
    { return {PreferencesWidget::Table, 0, 0, (child == WIDGET_CHILD), WidgetConfig (), table}; }

constexpr const PreferencesWidget WidgetNotebook (WidgetVNotebook notebook)
    { return {PreferencesWidget::Notebook, 0, 0, 0, WidgetConfig (), notebook}; }

constexpr const PreferencesWidget WidgetSeparator (WidgetVSeparator separator = WidgetVSeparator ())
    { return {PreferencesWidget::Separator, 0, 0, 0, WidgetConfig (), separator}; }

constexpr const PreferencesWidget WidgetCustom (void * (* populate) (void))
    { return {PreferencesWidget::Custom, 0, 0, 0, WidgetConfig (), populate}; }

struct PluginPreferences {
    ArrayRef<const PreferencesWidget> widgets;

    void (* init) (void);
    void (* apply) (void);
    void (* cleanup) (void);
};

#endif /* LIBAUDCORE_PREFERENCES_H */
