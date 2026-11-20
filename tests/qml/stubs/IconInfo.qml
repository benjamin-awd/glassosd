// SPDX-FileCopyrightText: 2026 glassosd contributors
// SPDX-License-Identifier: GPL-2.0-or-later
pragma Singleton
import QtQuick
// The real one renders icons to look at their pixels; tests have no theme.
QtObject {
    function prefersApp(url) { return false }
}
