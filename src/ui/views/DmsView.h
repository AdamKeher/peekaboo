/*
 * Copyright (c) 2026 Admin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <string>
#include "../../core/DmsParser.h"

namespace adm::ui {

/**
 * @brief DMS archive metadata, embedded as tabs in the document window.
 *
 * Parses a .dms file's header/track info and emits General / Track Information /
 * Banner / FILEID.DIZ tab items directly into the document window's tab bar
 * (FileListView), so the DMS details are self-contained per document rather than
 * a separate floating panel.
 */
class DmsView
{
public:
    DmsView() = default;
    ~DmsView() = default;

    void SetData(const std::string& path);
    bool HasData() const { return m_info.isValid; }

    // Emits the DMS tab items into the caller's existing tab bar (no window or
    // BeginTabBar of its own). No-op when no valid DMS data is loaded.
    void DrawTabItems();

private:
    void DrawGeneralTab();
    void DrawTrackInfoTab();
    void DrawBannerTab();
    void DrawFileIdDizTab();

    adm::core::DmsInfo m_info;
    std::string m_currentPath;
};

} // namespace adm::ui
