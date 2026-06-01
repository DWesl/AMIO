// grib2_driver.cpp -- AMIO GRIB2_Driver implementation.
//
// Implements the GRIB2 backend driver using nceplibs-g2c for encoding
// and decoding GRIB2 records.  Registers with BackendFactory under
// key "grib2" at static initialization time.
//
// Table sourcing
// --------------
// All GRIB2 code/template tables come from NCEPLIBS-g2c itself.  The
// encoding path calls the legacy g2c packing API:
//
//   g2_create()  -> Sections 0 & 1 (indicator + identification)
//   g2_addgrid() -> Section 3 (grid definition); g2c looks up the
//                   Grid Definition Template layout in its NCEP table
//                   via getgridtemplate()
//   g2_addfield()-> Sections 4/5/6/7 (product, data representation,
//                   bitmap, data); g2c looks up the Product- and Data-
//                   Representation-Template layouts via getpdstemplate()
//                   / getdrstemplate()
//   g2_gribend() -> Section 8 (end)
//
// AMIO supplies only the *numeric* code-table values (discipline,
// parameter category/number, template numbers, fixed-surface
// descriptors) from the manifest.  Those values ARE the WMO/NCEP
// table entries and are forwarded to g2c verbatim -- there is no
// eckit-authored string->code map anymore.
//
// Conditional compilation:
//   - AMIO_HAS_G2C: when defined, uses the real g2c API.
//   - AMIO_HAS_ECKIT: when defined, uses eckit::Exception and
//     eckit::Configuration.  Otherwise falls back to std::runtime_error.
//   - When g2c is not defined, the driver compiles but throws on
//     construction indicating g2c is not available.
//
// Validates: R9.1, R9.2, R9.3, R9.4, R9.5, R9.6, R9.7, R9.8

#include "drivers/grib2/grib2_driver.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "factory/backend_factory.hpp"
#include "staging/staging_pool.hpp"

#ifdef AMIO_HAS_G2C
// g2c's grib2.h has no extern "C" guard, so wrap it.  Pull in its
// standard C header dependencies first (outside the extern "C" block)
// so their include guards keep them from being processed under C
// linkage.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
extern "C" {
#include <grib2.h>  // nceplibs-g2c public header (g2int, g2_create, ...)
}
#endif

// When eckit is available, use eckit::Exception and eckit::Configuration.
// Otherwise, fall back to std::runtime_error for exception semantics.
#ifdef AMIO_HAS_ECKIT
#include <eckit/config/Configuration.h>
#include <eckit/config/LocalConfiguration.h>
#include <eckit/exception/Exceptions.h>
#include <eckit/log/Log.h>
#else
// Minimal shim: eckit::Configuration is forward-declared in
// backend_driver.hpp.  For compilation without eckit, we provide a
// minimal local configuration stub that satisfies the interface.
namespace eckit {

class Exception : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

class Configuration {
   public:
    virtual ~Configuration() = default;
    virtual bool has(const std::string& /*key*/) const {
        return false;
    }
    virtual bool getBool(const std::string& /*key*/, bool def) const {
        return def;
    }
    virtual int getInt(const std::string& /*key*/, int def) const {
        return def;
    }
    virtual long getLong(const std::string& /*key*/, long def) const {
        return def;
    }
    virtual long long getLong(const std::string& /*key*/, long long def) const {
        return def;
    }
    virtual std::string getString(const std::string& key) const {
        throw Exception("Key not found: " + key);
    }
    virtual std::string getString(const std::string& /*key*/, const std::string& def) const {
        return def;
    }
    virtual std::vector<std::string> getStringVector(const std::string& /*key*/) const {
        return {};
    }
    virtual std::vector<std::string> getStringVector(const std::string& /*key*/, const std::vector<std::string>& def) const {
        return def;
    }
};

namespace Log {
inline std::ostream& info() {
    return std::cerr;
}
inline std::ostream& warning() {
    return std::cerr;
}
inline std::ostream& error() {
    return std::cerr;
}
}  // namespace Log

}  // namespace eckit
#endif

namespace amio::detail {

// ---------------------------------------------------------------
// Static factory registration
// ---------------------------------------------------------------

namespace {
// Register GRIB2_Driver with BackendFactory under key "grib2" at
// static initialization time (R4.2, R4.5).
BackendRegistrar<GRIB2_Driver> grib2_registrar("grib2");

// Read a single int64 manifest field, falling back to `def` when the
// key is absent.  Centralizes the long/int getter quirks across the
// eckit and shim Configuration variants.
std::int64_t cfg_int(const eckit::Configuration& config, const std::string& key, std::int64_t def) {
    if (!config.has(key)) {
        return def;
    }
    return static_cast<std::int64_t>(config.getLong(key, static_cast<long>(def)));
}
}  // anonymous namespace

// ---------------------------------------------------------------
// DRT name parsing
// ---------------------------------------------------------------

GRIB2_DRT parse_drt_name(const std::string& name) {
    // Normalize to lowercase for comparison.
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    if (lower == "adaptive_entropy_coding" || lower == "libaec" || lower == "adaptive entropy coding via libaec" ||
        lower == "adaptive entropy coding") {
        return GRIB2_DRT::AdaptiveEntropyCoding;
    }
    if (lower == "lossless_jpeg2000" || lower == "jpeg2000" || lower == "lossless jpeg2000") {
        return GRIB2_DRT::LosslessJPEG2000;
    }

    // Not in the allowed set -- this is an unrecognized DRT (R9.6, R9.7).
    throw eckit::Exception("GRIB2_Driver: unrecognized Data Representation Template: '" + name +
                           "'. DRT name is not recognized. "
                           "Allowed values: {Adaptive Entropy Coding via libaec, "
                           "Lossless JPEG2000}. Zero record bytes emitted.");
}

// ---------------------------------------------------------------
// GRIB2_Driver construction / destruction
// ---------------------------------------------------------------

GRIB2_Driver::GRIB2_Driver() {
#ifndef AMIO_HAS_G2C
    // g2c is not available in this build.  Throw on construction
    // so the factory knows this driver cannot be used.
    throw eckit::Exception(
        "GRIB2_Driver: nceplibs-g2c is not available in this build. "
        "Rebuild AMIO with AMIO_HAS_G2C=ON to use the GRIB2 backend.");
#endif
}

GRIB2_Driver::~GRIB2_Driver() {
    if (initialized_) {
        // Best-effort cleanup; close() should have been called.
        try {
            close();
        } catch (...) {
            // Suppress exceptions in destructor.
        }
    }
}

// ---------------------------------------------------------------
// read_settings -- pull the numeric GRIB2 identifiers from manifest
// ---------------------------------------------------------------

Grib2Settings GRIB2_Driver::read_settings(const eckit::Configuration& config) {
    Grib2Settings s{};  // start from NCEP-flavored defaults

    s.discipline = cfg_int(config, "grib2.discipline", s.discipline);
    s.center = cfg_int(config, "grib2.center", s.center);
    s.subcenter = cfg_int(config, "grib2.subcenter", s.subcenter);
    s.master_table_version = cfg_int(config, "grib2.master_table_version", s.master_table_version);
    s.local_table_version = cfg_int(config, "grib2.local_table_version", s.local_table_version);
    s.significance_of_ref_time = cfg_int(config, "grib2.significance_of_ref_time", s.significance_of_ref_time);
    s.production_status = cfg_int(config, "grib2.production_status", s.production_status);
    s.type_of_data = cfg_int(config, "grib2.type_of_data", s.type_of_data);

    s.gdt_number = cfg_int(config, "grib2.gdt_number", s.gdt_number);
    s.lat_first = cfg_int(config, "grib2.lat_first", s.lat_first);
    s.lon_first = cfg_int(config, "grib2.lon_first", s.lon_first);
    s.lat_last = cfg_int(config, "grib2.lat_last", s.lat_last);
    s.lon_last = cfg_int(config, "grib2.lon_last", s.lon_last);

    s.pdt_number = cfg_int(config, "grib2.pdt_number", s.pdt_number);
    // Accept both the precise names and the short aliases used in the
    // example manifests ("category"/"parameter").
    s.parameter_category = cfg_int(config, "grib2.parameter_category", s.parameter_category);
    s.parameter_category = cfg_int(config, "grib2.category", s.parameter_category);
    s.parameter_number = cfg_int(config, "grib2.parameter_number", s.parameter_number);
    s.parameter_number = cfg_int(config, "grib2.parameter", s.parameter_number);
    s.type_of_first_fixed_surface = cfg_int(config, "grib2.type_of_first_fixed_surface", s.type_of_first_fixed_surface);
    s.scale_factor_first_surface = cfg_int(config, "grib2.scale_factor_first_surface", s.scale_factor_first_surface);
    s.scaled_value_first_surface = cfg_int(config, "grib2.scaled_value_first_surface", s.scaled_value_first_surface);
    s.forecast_time = cfg_int(config, "grib2.forecast_time", s.forecast_time);
    s.indicator_of_unit_of_time = cfg_int(config, "grib2.indicator_of_unit_of_time", s.indicator_of_unit_of_time);

    s.decimal_scale_factor = cfg_int(config, "grib2.decimal_scale_factor", s.decimal_scale_factor);

    return s;
}

// ---------------------------------------------------------------
// open_write / open_read
// ---------------------------------------------------------------

void GRIB2_Driver::open_write(const eckit::Configuration& config) {
    active_drt_ = validate_drt(config);
    initialize(config);
}

void GRIB2_Driver::open_read(const eckit::Configuration& config) {
    initialize(config);
}

// ---------------------------------------------------------------
// initialize -- read product identifiers + open output file
// ---------------------------------------------------------------

void GRIB2_Driver::initialize(const eckit::Configuration& config) {
    if (initialized_) {
        return;  // Already initialized.
    }

#ifndef AMIO_HAS_G2C
    (void)config;
    throw eckit::Exception("GRIB2_Driver: nceplibs-g2c is not available in this build.");
#else
    // The GRIB2 tables are provided by g2c itself; AMIO only needs the
    // numeric product identifiers from the manifest.
    settings_ = read_settings(config);

    // Resolve the output path (write mode only).
    if (config.has("path")) {
        output_path_ = config.getString("path");
    } else if (config.has("output_path")) {
        output_path_ = config.getString("output_path");
    }

    if (!output_path_.empty()) {
        out_file_ = std::fopen(output_path_.c_str(), "wb");
        if (out_file_ == nullptr) {
            throw eckit::Exception("GRIB2_Driver: failed to open output file '" + output_path_ + "' for writing.");
        }
    }

    initialized_ = true;
#endif  // AMIO_HAS_G2C
}

// ---------------------------------------------------------------
// NCEP template builders
//
// These populate the g2c template-value arrays.  g2c consults its
// own NCEP template tables (getgridtemplate / getpdstemplate /
// getdrstemplate) for the matching layout; we only supply values.
// ---------------------------------------------------------------

std::vector<std::int64_t> GRIB2_Driver::build_gdt_3_0(const Grib2Settings& s, std::int64_t ni, std::int64_t nj) {
    // Grid Definition Template 3.0 (regular lat/lon), 19 entries.
    // Layout per WMO GRIB2 Template 3.0 / NCEP getgridtemplate(0).
    std::vector<std::int64_t> t(19, 0);
    t[0] = 6;             // shape of earth (6 = sphere, radius 6,371,229 m)
    t[1] = 0;             // scale factor of radius of spherical earth
    t[2] = 0;             // scaled value of radius of spherical earth
    t[3] = 0;             // scale factor of major axis
    t[4] = 0;             // scaled value of major axis
    t[5] = 0;             // scale factor of minor axis
    t[6] = 0;             // scaled value of minor axis
    t[7] = ni;            // Ni - number of points along a parallel
    t[8] = nj;            // Nj - number of points along a meridian
    t[9] = 0;             // basic angle of initial production domain
    t[10] = 0;            // subdivisions of basic angle (missing = all 1s -> 0 here)
    t[11] = s.lat_first;  // La1 (1e-6 deg)
    t[12] = s.lon_first;  // Lo1 (1e-6 deg)
    t[13] = 48;           // resolution and component flags (0x30: i,j dir incr given)
    t[14] = s.lat_last;   // La2 (1e-6 deg)
    t[15] = s.lon_last;   // Lo2 (1e-6 deg)
    // Di / Dj direction increments (1e-6 deg), derived from the span.
    std::int64_t di = (ni > 1) ? ((s.lon_last - s.lon_first) / (ni - 1)) : 0;
    std::int64_t dj = (nj > 1) ? ((s.lat_first - s.lat_last) / (nj - 1)) : 0;
    t[16] = di < 0 ? -di : di;  // Di must be positive
    t[17] = dj < 0 ? -dj : dj;  // Dj must be positive
    t[18] = 0;                  // scanning mode (0 = +i, -j, row-major from NW corner)
    return t;
}

std::vector<std::int64_t> GRIB2_Driver::build_pdt_4_0(const Grib2Settings& s) {
    // Product Definition Template 4.0, 15 entries.
    std::vector<std::int64_t> t(15, 0);
    t[0] = s.parameter_category;           // Table 4.1
    t[1] = s.parameter_number;             // Table 4.2
    t[2] = 2;                              // generating process type (2 = forecast)
    t[3] = 0;                              // background generating process id
    t[4] = 0;                              // analysis/forecast generating process id
    t[5] = 0;                              // hours after reference time (data cutoff)
    t[6] = 0;                              // minutes after reference time (data cutoff)
    t[7] = s.indicator_of_unit_of_time;    // Table 4.4
    t[8] = s.forecast_time;                // forecast time in above units
    t[9] = s.type_of_first_fixed_surface;  // Table 4.5
    t[10] = s.scale_factor_first_surface;  // scale factor of first fixed surface
    t[11] = s.scaled_value_first_surface;  // scaled value of first fixed surface
    t[12] = 255;                           // type of second fixed surface (255 = missing)
    t[13] = 0;                             // scale factor of second fixed surface
    t[14] = 0;                             // scaled value of second fixed surface
    return t;
}

std::vector<std::int64_t> GRIB2_Driver::build_drs_template(GRIB2_DRT drt, const Grib2Settings& s) {
    if (drt == GRIB2_DRT::LosslessJPEG2000) {
        // Data Representation Template 5.40 (JPEG2000), 7 entries.
        // Entry 0 (reference value) is filled by g2c during packing.
        std::vector<std::int64_t> t(7, 0);
        t[1] = 0;                       // binary scale factor
        t[2] = s.decimal_scale_factor;  // decimal scale factor
        t[3] = 0;                       // number of bits (0 = g2c chooses)
        t[4] = 0;                       // original field type (0 = floating point)
        t[5] = 0;                       // compression type (0 = lossless)
        t[6] = 255;                     // target compression ratio (255 = lossless)
        return t;
    }
    // Data Representation Template 5.42 (CCSDS / AEC via libaec), 8 entries.
    std::vector<std::int64_t> t(8, 0);
    t[1] = 0;                       // binary scale factor
    t[2] = s.decimal_scale_factor;  // decimal scale factor
    t[3] = 0;                       // number of bits (0 = g2c chooses)
    t[4] = 0;                       // original field type (0 = floating point)
    t[5] = 0;                       // CCSDS compression options mask
    t[6] = 0;                       // CCSDS block size
    t[7] = 0;                       // CCSDS reference sample interval
    return t;
}

// ---------------------------------------------------------------
// write -- encode a GRIB2 record (R9.3, R9.4, R9.5, R9.6, R9.7, R9.8)
// ---------------------------------------------------------------

void GRIB2_Driver::write(const StagingBuffer& src, const VarMeta& meta) {
    if (!initialized_) {
        throw eckit::Exception("GRIB2_Driver::write called before successful initialization");
    }

    // GRIB2 grid-point encoding operates on float fields.  Only F32
    // payloads are supported; anything else is rejected before any
    // bytes are emitted (R9.8).
    if (meta.dtype != AMIO_DTYPE_F32) {
        throw eckit::Exception(
            "GRIB2_Driver::write: only AMIO_DTYPE_F32 fields can be encoded "
            "to GRIB2 grid-point data. Zero record bytes emitted.");
    }

    const std::size_t elem_size = dtype_size(meta.dtype);
    const std::size_t num_elements = total_elements(meta.shape);
    if (num_elements == 0 || elem_size == 0) {
        throw eckit::Exception("GRIB2_Driver::write: empty or invalid field shape. Zero record bytes emitted.");
    }

    // Contiguity check gates fast vs slow path (R9.4, R9.5).
    const std::byte* encode_ptr = nullptr;
    std::vector<std::byte> packed_buffer;
    if (is_contiguous_row_major(meta.shape)) {
        // Fast path: pass the staging buffer through directly.
        encode_ptr = src.data;
    } else {
        // Slow path: pack into a contiguous row-major buffer.
        packed_buffer = pack_row_major(src.data, meta.shape, elem_size);
        encode_ptr = packed_buffer.data();
    }

#ifdef AMIO_HAS_G2C
    // Derive the grid dimensions.  For rank >= 2 the last dimension is
    // longitudes (Ni, fastest varying) and the second-to-last is
    // latitudes (Nj).  For rank 1 we treat the field as a single row.
    std::int64_t ni = 0;
    std::int64_t nj = 0;
    if (meta.shape.rank >= 2) {
        nj = meta.shape.extents[meta.shape.rank - 2];
        ni = meta.shape.extents[meta.shape.rank - 1];
    } else {
        nj = 1;
        ni = static_cast<std::int64_t>(num_elements);
    }

    // Section 0 (indicator): [discipline, edition].
    g2int listsec0[2];
    listsec0[0] = static_cast<g2int>(settings_.discipline);
    listsec0[1] = 2;  // GRIB edition 2

    // Section 1 (identification): 13 entries.
    g2int listsec1[13];
    listsec1[0] = static_cast<g2int>(settings_.center);
    listsec1[1] = static_cast<g2int>(settings_.subcenter);
    listsec1[2] = static_cast<g2int>(settings_.master_table_version);
    listsec1[3] = static_cast<g2int>(settings_.local_table_version);
    listsec1[4] = static_cast<g2int>(settings_.significance_of_ref_time);
    listsec1[5] = 1970;  // year   (reference time; AMIO does not yet thread real dates)
    listsec1[6] = 1;     // month
    listsec1[7] = 1;     // day
    listsec1[8] = 0;     // hour
    listsec1[9] = 0;     // minute
    listsec1[10] = 0;    // second
    listsec1[11] = static_cast<g2int>(settings_.production_status);
    listsec1[12] = static_cast<g2int>(settings_.type_of_data);

    // Generously size the GRIB message buffer: header + raw field +
    // slack for section overhead.  g2c writes into this buffer.
    const std::size_t field_bytes = num_elements * sizeof(float);
    std::vector<unsigned char> cgrib(field_bytes + 8192 + num_elements);

    g2int ierr = g2_create(cgrib.data(), listsec0, listsec1);
    if (ierr <= 0) {
        throw eckit::Exception("GRIB2_Driver::write: g2_create failed (Section 0/1). Zero record bytes emitted.");
    }

    // Section 3 (grid definition).  igds describes the source of the
    // grid definition and the number of data points.
    auto gdt = build_gdt_3_0(settings_, ni, nj);
    std::vector<g2int> igdstmpl(gdt.begin(), gdt.end());
    g2int igds[5];
    igds[0] = 0;                                         // grid defined by template (Table 3.0)
    igds[1] = static_cast<g2int>(ni * nj);               // number of data points
    igds[2] = 0;                                         // octets for optional list of numbers
    igds[3] = 0;                                         // interpretation of optional list
    igds[4] = static_cast<g2int>(settings_.gdt_number);  // Grid Definition Template number

    ierr = g2_addgrid(cgrib.data(), igds, igdstmpl.data(), nullptr, 0);
    if (ierr <= 0) {
        throw eckit::Exception("GRIB2_Driver::write: g2_addgrid failed (Section 3). Zero record bytes emitted.");
    }

    // Sections 4/5/6/7 (product, data representation, bitmap, data).
    auto pdt = build_pdt_4_0(settings_);
    std::vector<g2int> ipdstmpl(pdt.begin(), pdt.end());
    auto drs = build_drs_template(active_drt_, settings_);
    std::vector<g2int> idrstmpl(drs.begin(), drs.end());

    // Copy the (already row-major) payload into a float field array.
    std::vector<float> fld(num_elements);
    std::memcpy(fld.data(), encode_ptr, num_elements * sizeof(float));

    g2int idrsnum = static_cast<g2int>(active_drt_);  // 40 or 42
    ierr = g2_addfield(cgrib.data(),
                       static_cast<g2int>(settings_.pdt_number),  // Product Definition Template number
                       ipdstmpl.data(),
                       nullptr,  // coordlist
                       0,        // numcoord
                       idrsnum, idrstmpl.data(), fld.data(), static_cast<g2int>(num_elements),
                       0,         // ibmap = 0 -> bitmap applies (255 would mean none); 0 uses supplied bmap
                       nullptr);  // bmap (none)
    if (ierr <= 0) {
        throw eckit::Exception("GRIB2_Driver::write: g2_addfield failed (Section 4/5/7). Zero record bytes emitted.");
    }

    g2int msglen = g2_gribend(cgrib.data());
    if (msglen <= 0) {
        throw eckit::Exception("GRIB2_Driver::write: g2_gribend failed (Section 8). Zero record bytes emitted.");
    }

    // Commit the complete GRIB2 message to the output file.
    if (out_file_ != nullptr) {
        std::size_t written = std::fwrite(cgrib.data(), 1, static_cast<std::size_t>(msglen), out_file_);
        if (written != static_cast<std::size_t>(msglen)) {
            throw eckit::Exception("GRIB2_Driver::write: short write committing GRIB2 message to disk.");
        }
    }
#else
    (void)encode_ptr;
    throw eckit::Exception("GRIB2_Driver::write: nceplibs-g2c not available");
#endif
}

// ---------------------------------------------------------------
// read -- decode a GRIB2 record into StagingBuffer
// ---------------------------------------------------------------

void GRIB2_Driver::read(StagingBuffer& dst, const VarMeta& meta, std::int64_t timestep, const std::optional<BoundingBox>& bbox) {
    if (!initialized_) {
        throw eckit::Exception("GRIB2_Driver::read called before successful initialization");
    }

#ifdef AMIO_HAS_G2C
    // Decode is not yet implemented; the write path is the focus of
    // this change.  Tables for decode also come from g2c (g2_getfld).
    (void)meta;
    (void)timestep;
    (void)bbox;
    dst.used_bytes = 0;
#else
    (void)dst;
    (void)meta;
    (void)timestep;
    (void)bbox;
    throw eckit::Exception("GRIB2_Driver::read: nceplibs-g2c not available");
#endif
}

// ---------------------------------------------------------------
// flush -- flush the output file buffer
// ---------------------------------------------------------------

void GRIB2_Driver::flush() {
#ifdef AMIO_HAS_G2C
    if (out_file_ != nullptr) {
        std::fflush(out_file_);
    }
#endif
}

// ---------------------------------------------------------------
// close -- release file handle and g2c resources
// ---------------------------------------------------------------

void GRIB2_Driver::close() {
    if (!initialized_) {
        return;
    }

#ifdef AMIO_HAS_G2C
    if (out_file_ != nullptr) {
        std::fclose(out_file_);
        out_file_ = nullptr;
    }
#endif

    initialized_ = false;
}

// ---------------------------------------------------------------
// validate_drt -- check DRT field is present and in allowed set
// (R9.6, R9.7)
// ---------------------------------------------------------------

GRIB2_DRT GRIB2_Driver::validate_drt(const eckit::Configuration& config) const {
    // Check if DRT field is present (R9.7 - "missing" case).
    if (!config.has("data_representation_template") && !config.has("drt")) {
        throw eckit::Exception(
            "GRIB2_Driver: Data Representation Template field is missing "
            "from configuration. Required field: 'data_representation_"
            "template' or 'drt'. The field was missing. "
            "Zero record bytes emitted.");
    }

    // Get the DRT name.
    std::string drt_name;
    if (config.has("data_representation_template")) {
        drt_name = config.getString("data_representation_template");
    } else {
        drt_name = config.getString("drt");
    }

    if (drt_name.empty()) {
        throw eckit::Exception(
            "GRIB2_Driver: Data Representation Template field is present "
            "but empty. The field value is missing. "
            "Required: one of {Adaptive Entropy Coding via libaec, "
            "Lossless JPEG2000}. Zero record bytes emitted.");
    }

    // parse_drt_name throws for unrecognized names (R9.6, R9.7 -
    // "unrecognized" case).
    return parse_drt_name(drt_name);
}

// ---------------------------------------------------------------
// is_contiguous_row_major -- check if shape describes contiguous
// row-major layout (R9.4)
// ---------------------------------------------------------------

bool GRIB2_Driver::is_contiguous_row_major(const amio_shape_t& shape) {
    if (shape.rank <= 0 || shape.rank > AMIO_MAX_RANK) {
        return false;
    }

    // If all strides are 0, the layout is contiguous row-major by
    // convention (the shape descriptor asks AMIO to derive strides
    // from extents).  This is equivalent to is_always_contiguous()
    // returning true with row-major layout.
    bool all_strides_zero = true;
    for (std::int32_t d = 0; d < shape.rank; ++d) {
        if (shape.strides[d] != 0) {
            all_strides_zero = false;
            break;
        }
    }
    if (all_strides_zero) {
        return true;
    }

    // Verify explicit strides match row-major contiguous layout.
    // Row-major: stride[rank-1] = 1, stride[d] = product(extents[d+1..rank-1])
    std::int64_t expected_stride = 1;
    for (std::int32_t d = shape.rank - 1; d >= 0; --d) {
        if (shape.strides[d] != 0 && shape.strides[d] != expected_stride) {
            return false;
        }
        expected_stride *= shape.extents[d];
    }

    return true;
}

// ---------------------------------------------------------------
// pack_row_major -- pack non-contiguous data into contiguous buffer
// (R9.5)
// ---------------------------------------------------------------

std::vector<std::byte> GRIB2_Driver::pack_row_major(const std::byte* src_data, const amio_shape_t& shape, std::size_t element_size) {
    const std::size_t num_elements = total_elements(shape);
    std::vector<std::byte> packed(num_elements * element_size);

    if (num_elements == 0 || element_size == 0) {
        return packed;
    }

    // Compute actual strides (resolve zeros to row-major defaults).
    std::int64_t actual_strides[AMIO_MAX_RANK] = {};
    std::int64_t row_major_stride = 1;
    for (std::int32_t d = shape.rank - 1; d >= 0; --d) {
        if (shape.strides[d] == 0) {
            actual_strides[d] = row_major_stride;
        } else {
            actual_strides[d] = shape.strides[d];
        }
        row_major_stride *= shape.extents[d];
    }

    // Iterate over all elements in row-major order and copy each
    // element from its strided position to the packed buffer.
    std::int64_t indices[AMIO_MAX_RANK] = {};
    std::size_t dst_offset = 0;

    for (std::size_t elem = 0; elem < num_elements; ++elem) {
        // Compute source offset from indices and strides (in elements).
        std::size_t src_elem_offset = 0;
        for (std::int32_t d = 0; d < shape.rank; ++d) {
            src_elem_offset += static_cast<std::size_t>(indices[d]) * static_cast<std::size_t>(actual_strides[d]);
        }
        std::size_t src_byte_offset = src_elem_offset * element_size;

        // Copy one element.
        std::memcpy(packed.data() + dst_offset, src_data + src_byte_offset, element_size);
        dst_offset += element_size;

        // Increment multi-dimensional index (row-major: last dim first).
        for (std::int32_t d = shape.rank - 1; d >= 0; --d) {
            indices[d]++;
            if (indices[d] < shape.extents[d]) {
                break;
            }
            indices[d] = 0;
        }
    }

    return packed;
}

// ---------------------------------------------------------------
// total_elements -- compute total element count from shape
// ---------------------------------------------------------------

std::size_t GRIB2_Driver::total_elements(const amio_shape_t& shape) {
    if (shape.rank <= 0 || shape.rank > AMIO_MAX_RANK) {
        return 0;
    }
    std::size_t count = 1;
    for (std::int32_t d = 0; d < shape.rank; ++d) {
        if (shape.extents[d] <= 0) {
            return 0;
        }
        count *= static_cast<std::size_t>(shape.extents[d]);
    }
    return count;
}

// ---------------------------------------------------------------
// dtype_size -- element size in bytes from dtype tag
// ---------------------------------------------------------------

std::size_t GRIB2_Driver::dtype_size(amio_dtype_t dtype) {
    // Delegate to the shared element_size helper (backend_driver.hpp); it
    // already returns 0 as the unknown-dtype sentinel that this driver relies
    // on (see read()'s elem_size == 0 guard).
    return element_size(dtype);
}

}  // namespace amio::detail
