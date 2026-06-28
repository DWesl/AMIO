// var_attributes.cpp -- implementation of the CF/UGRID attribute model.
//
// See var_attributes.hpp for the contract.  The manifest parsing uses
// HELM::CONF's typed accessors and dotted-path API to walk the
// configuration tree.

#include "drivers/common/var_attributes.hpp"

#include <cctype>
#include <conf/config.hpp>
#include <cstdlib>
#include <string>

namespace amio::detail {

// ---------------------------------------------------------------
// parse_attr_value -- detect numeric literals; keep original text.
// ---------------------------------------------------------------

AttrValue parse_attr_value(const std::string &raw) {
    AttrValue v;
    v.text = raw;
    v.is_numeric = false;
    v.is_integer = false;

    if (raw.empty()) {
        return v;
    }

    // Attempt a full-string numeric parse via std::strtod.  Only treat
    // the value as numeric when the entire (trimmed) string is consumed.
    const char *begin = raw.c_str();
    char *end = nullptr;
    errno = 0;
    double d = std::strtod(begin, &end);

    // Skip trailing whitespace after the parsed number.
    while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }

    if (end != nullptr && *end == '\0' && end != begin) {
        v.is_numeric = true;
        v.number = d;
        // Integer iff no decimal point / exponent appears in the text.
        v.is_integer = raw.find('.') == std::string::npos && raw.find('e') == std::string::npos && raw.find('E') == std::string::npos;
    }

    return v;
}

namespace {

// Detect a UGRID role in a variable's attribute set.
bool has_ugrid_role(const VarAttributes &attrs) {
    for (const auto &kv : attrs.items) {
        if (kv.first == "cf_role" || kv.first == "topology_dimension" || kv.first == "mesh_topology") {
            return true;
        }
    }
    return false;
}

}  // namespace

DatasetAttributes parse_dataset_attributes(const conf::Config &config) {
    DatasetAttributes out;

    // Global extra attributes: read known attribute keys under
    // "global_attributes.*".  CONF doesn't expose keys() iteration
    // over a sub-map directly, so we check for commonly used global
    // attribute keys.  If the manifest uses a flat structure, we can
    // read them by dotted path.
    //
    // For a fully dynamic approach, the attribute parsing would need
    // CONF's Value/node iteration API.  For now, we attempt to read
    // common CF global attributes via try_string.
    if (config.has("global_attributes")) {
        // Read known global attribute keys that CF mandates or commonly uses.
        static const char *known_global_keys[] = {"title",   "institution", "source",  "history", "references",
                                                  "comment", "Conventions", "contact", "project"};
        for (const char *key : known_global_keys) {
            std::string dotted = std::string("global_attributes.") + key;
            auto val = config.try_string(dotted);
            if (val.has_value()) {
                out.global.set(key, parse_attr_value(*val));
            }
        }
    }

    // Per-variable attributes: CONF's dotted-path API requires knowing
    // variable names in advance.  The driver typically knows its own
    // variable name(s) and can query per-variable attributes at write
    // time.  For the attribute model construction, we rely on the
    // manifest listing variable names via a string list.
    if (config.has("variable_names")) {
        try {
            auto var_names = config.get_string_list("variable_names");
            for (const std::string &var_name : var_names) {
                std::string prefix = "variables." + var_name + ".attributes";
                if (!config.has(prefix)) {
                    continue;
                }
                VarAttributes attrs;
                // Read known CF/UGRID per-variable attribute keys.
                static const char *known_var_keys[] = {"units",        "long_name",  "standard_name", "_FillValue", "coordinates",
                                                       "cell_methods", "cf_role",    "mesh",          "location",   "topology_dimension",
                                                       "scale_factor", "add_offset", "valid_min",     "valid_max",  "valid_range"};
                for (const char *key : known_var_keys) {
                    std::string dotted = prefix + "." + key;
                    auto val = config.try_string(dotted);
                    if (val.has_value()) {
                        attrs.set(key, parse_attr_value(*val));
                    }
                }
                if (has_ugrid_role(attrs)) {
                    out.uses_ugrid = true;
                }
                if (!attrs.empty()) {
                    out.per_variable.emplace(var_name, std::move(attrs));
                }
            }
        } catch (...) {
            // Malformed block -- ignore; per-variable map stays as-is.
        }
    }

    // Resolve the Conventions string: explicit manifest value wins;
    // otherwise default to CF, upgraded to CF+UGRID when a mesh role
    // was declared.
    if (config.has("conventions")) {
        out.conventions = config.get_or<std::string>("conventions", kDefaultCFConventions);
    } else {
        out.conventions = out.uses_ugrid ? kDefaultCFUGRIDConventions : kDefaultCFConventions;
    }

    return out;
}

}  // namespace amio::detail
