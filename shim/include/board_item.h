// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's BOARD_ITEM (include/board_item.h). The router core
// keeps a BOARD_ITEM* per PNS item (ITEM::Parent()) and reads only its type,
// its layer, whether it is connected, and (the router's debug logger) its
// uuid. The service's own parents derive from these classes (src/engine.cpp).
#pragma once

#include <initializer_list>

#include <core/typeinfo.h>
#include <kiid.h>
#include <layer_ids.h>
#include <units_provider.h>
#include <wx/string.h>

class BOARD;

class BOARD_ITEM {
public:
    explicit BOARD_ITEM(KICAD_T aType, PCB_LAYER_ID aLayer = UNDEFINED_LAYER) : m_type(aType), m_layer(aLayer) {}
    virtual ~BOARD_ITEM() = default;

    KICAD_T Type() const { return m_type; }
    bool IsType(std::initializer_list<KICAD_T> aTypes) const
    {
        for (KICAD_T t : aTypes)
            if (t == m_type)
                return true;
        return false;
    }
    PCB_LAYER_ID GetLayer() const { return m_layer; }
    bool IsOnCopperLayer() const { return IsCopperLayer(m_layer); }
    virtual bool IsConnected() const { return false; }
    // debug messages only
    virtual wxString GetItemDescription(UNITS_PROVIDER*, bool) const { return wxString("item"); }

    const KIID m_Uuid;

protected:
    KICAD_T m_type;
    PCB_LAYER_ID m_layer;
};
