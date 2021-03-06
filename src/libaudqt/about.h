/*
 * about.cc
 * Copyright 2014 William Pitcock
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

#include <QtGui>
#include <QtWidgets>

#include <libaudcore/audstrings.h>
#include <libaudcore/i18n.h>
#include <libaudcore/runtime.h>

#include "libaudqt.h"

#ifndef LIBAUDQT_ABOUT_H
#define LIBAUDQT_ABOUT_H

namespace audqt {

class AboutWindow : public QDialog {

    Q_OBJECT

private:
    QVBoxLayout m_layout;
    QTabWidget m_tabs;
    QLabel m_logo;
    QLabel m_about_text;
    QPlainTextEdit *m_textedits[2];

    void buildCreditsNotebook ();

public:
    AboutWindow (QWidget * parent = 0);
    ~AboutWindow ();
};

};

#endif
