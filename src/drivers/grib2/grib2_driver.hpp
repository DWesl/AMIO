// grib2_driver.hpp -- AMIO GRIB2_Driver backend implementation.
//
// This header is PRIVATE to the AMIO_Core build (`src/drivers/grib2/`).
// It is never installed and must not be referenced from any public
// header under `include/amio/`.
//
// Purpose
// -------
// The GRIB2_Driver encodes/decodes model output to WMO-compliant
// GRIB2 using nceplibs-g2c.  Key design features:
//
//   * GRIB2 code/template tables are sourced *directly from
//     NCEPLIBS-g2c* (the canonical NCEP tables shipped with the
//     library).  The encoder calls g2_create / g2_addgrid /
//     g2_addfield / g2_gribend, which internally consult g2c's
//     getgridtemplate() / getpdstemplate() / getdrstemplate()
//     NCEP template tables to lay out each section.  AMIO does NOT
//     maintain its own table file and does NOT translate metadata
//     through an eckit-authored code map.
//
//   * The manifest supplies the *numeric* GRIB2 product identifiers
//     (discipline, parameter category/number, grid- and product-
//     definition template numbers, fixed-surface descriptors).
//     These ARE the WMO/NCEP code-table values; they are passed
//     straight through to g2c without remapping.
//
//   * Mdspan contiguity check gates fast vs slow path:
//       - is_always_contiguous() + row-major -> zero-copy flatten
//       - Otherwise -> pack into a contiguous row-major buffer
//
//   * DRT restricted to {Adaptive Entropy Coding via libaec (42),
//     Lossless JPEG2000 (40)}; others rejected with eckit::Exception.
//   * Missing/invalid DRT or unsupported dtype -> eckit::Exception,
//     zero output bytes.
//
// Conditional compilation:
//   - AMIO_HAS_G2C: when defined, uses the real g2c API.
//   - AMIO_HAS_ECKIT: when defined, uses eckit::Exception and
//     eckit::Configuration.  Otherwise falls back to std::runtime_error.
//   - When g2c is not available, the driver compiles but throws on
//     construction indicating g2c is not available.
//
// Registered with BackendFactory via key "grib2" at static init.
//
// Thread safety
// -------------
// A single GRIB2_Driver instance is NOT thread-safe.  The Worker_Pool
// serializes calls via the per-(dataset, variable) ordering mutex.
//
// Validates: R9.1, R9.2, R9.3, R9.4, R9.5, R9.6, R9.7, R9.8

#ifndef AMIO_SRC_DRIVERS_GRIB2_GRIB2_DRIVER_HPP
#define AMIO_SRC_DRIVERS_GRIB2_GRIB2_DRIVER_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "factory/backend_driver.hpp"

namespace amio::detail {

// Forward-declare StagingPool for slow-path buffer allocation.
class StagingPool;

// Allowed Data Representation Template identifiers.
// These correspond to the WMO GRIB2 DRT numbers:
//   - 42 = Grid Point Data - CCSDS (Adaptive Entropy Coding via libaec)
//   - 40 = Grid Point Data - JPEG2000 (Lossless JPEG2000)
enum class GRIB2_DRT : std::int32_t {
    AdaptiveEntropyCoding = 42,  // libaec
    LosslessJPEG2000 = 40        // JPEG2000 lossless mode
};

// Map from human-readable DRT name to enum.
// Recognized names:
//   "adaptive_entropy_coding" or "libaec" -> AdaptiveEntropyCoding
//   "lossless_jpeg2000" or "jpeg2000"     -> LosslessJPEG2000
// Throws on unrecognized name.
GRIB2_DRT parse_drt_name(const std::string& name);

// Grib2Settings -- the numeric GRIB2 product identifiers read from
// the manifest.  Every field is a WMO/NCEP code-table value; the
// driver forwards them verbatim to g2c's NCEP template tables.
//
// Defaults describe a global regular lat/lon grid of a meteorological
// product authored by NCEP (center 7), which is the common case for
// NWP dissemination.  Any field may be overridden from the manifest's
// `grib2:` block.
struct Grib2Settings {
    // Section 0 (Indicator) / Section 1 (Identification).
    std::int64_t discipline = 0;  // Table 0.0 (0 = meteorological)
    std::int64_t center = 7;      // Table C-1 (7 = US NWS NCEP)
    std::int64_t subcenter = 0;
    std::int64_t master_table_version = 2;      // Table 1.0
    std::int64_t local_table_version = 1;       // Table 1.1
    std::int64_t significance_of_ref_time = 1;  // Table 1.2 (1 = start of forecast)
    std::int64_t production_status = 0;         // Table 1.3 (0 = operational)
    std::int64_t type_of_data = 1;              // Table 1.4 (1 = forecast)

    // Section 3 (Grid Definition).
    std::int64_t gdt_number = 0;  // Table 3.1 (0 = regular lat/lon)
    // Grid geometry in units of 1e-6 degrees.  Defaults span the globe.
    std::int64_t lat_first = 90000000;  // La1
    std::int64_t lon_first = 0;         // Lo1
    std::int64_t lat_last = -90000000;  // La2
    std::int64_t lon_last = 359000000;  // Lo2

    // Section 4 (Product Definition).
    std::int64_t pdt_number = 0;                     // Table 4.0 (0 = analysis/forecast at a level)
    std::int64_t parameter_category = 0;             // Table 4.1
    std::int64_t parameter_number = 0;               // Table 4.2
    std::int64_t type_of_first_fixed_surface = 100;  // Table 4.5 (100 = isobaric)
    std::int64_t scale_factor_first_surface = 0;
    std::int64_t scaled_value_first_surface = 0;  // e.g. 50000 Pa for 500 hPa
    std::int64_t forecast_time = 0;               // in units of indicator_of_unit_of_time
    std::int64_t indicator_of_unit_of_time = 1;   // Table 4.4 (1 = hour)

    // Section 5 (Data Representation) packing precision.
    std::int64_t decimal_scale_factor = 0;
};

// GRIB2_Driver -- concrete Backend_Driver for GRIB2 encoding via
// nceplibs-g2c.
//
// Lifecycle:
//   1. Default-constructed by BackendFactory.  When AMIO_HAS_G2C is
//      not defined, the constructor throws immediately indicating
//      g2c is unavailable.
//   2. open_write() / open_read() reads the GRIB2 product identifiers
//      and validates the DRT.  No external table file is loaded -- the
//      tables live inside g2c.
//   3. write() / read() encode/decode GRIB2 records.
//   4. flush() flushes the output file.
//   5. close() releases the file handle.
class GRIB2_Driver : public Backend_Driver {
   public:
    // Constructor.  When AMIO_HAS_G2C is not defined, throws
    // immediately indicating g2c is not available in this build.
    GRIB2_Driver();
    ~GRIB2_Driver() override;

    // Backend_Driver interface
    void open_write(const eckit::Configuration& config) override;
    void open_read(const eckit::Configuration& config) override;
    void write(const StagingBuffer& src, const VarMeta& meta) override;
    void read(StagingBuffer& dst, const VarMeta& meta, std::int64_t timestep, const std::optional<BoundingBox>& bbox) override;
    void flush() override;
    void close() override;

   private:
    // Read the GRIB2 product identifiers from the manifest and open
    // the output file.  Throws on failure.  Called from open_write.
    void initialize(const eckit::Configuration& config);

    // Validate that the DRT field is present and in the allowed set.
    // Throws with appropriate message on failure, identifying whether
    // the field was missing or the name was unrecognized (R9.7).
    GRIB2_DRT validate_drt(const eckit::Configuration& config) const;

   public:
    // ----- Static utility methods (public for testability) -----

    // Check if a buffer described by shape is contiguous and row-major.
    // Returns true if the data can be passed directly to g2c (fast path).
    // Implements the is_always_contiguous() + row-major check (R9.4).
    static bool is_contiguous_row_major(const amio_shape_t& shape);

    // Pack non-contiguous data into a contiguous row-major buffer (R9.5).
    // Returns the packed buffer as a vector of bytes.
    static std::vector<std::byte> pack_row_major(const std::byte* src_data, const amio_shape_t& shape, std::size_t element_size);

    // Compute total element count from shape.
    static std::size_t total_elements(const amio_shape_t& shape);

    // Compute element size in bytes from dtype.
    static std::size_t dtype_size(amio_dtype_t dtype);

    // ----- NCEP template builders (public for testability) -----
    //
    // Each returns a g2c template-value array (one std::int64_t per
    // template entry) populated from `s` and the grid dimensions.
    // These values are handed to g2c, which consults its NCEP tables
    // (getgridtemplate / getpdstemplate / getdrstemplate) for the
    // matching template *layout*.  The values themselves are the
    // WMO/NCEP code-table numbers carried straight through from the
    // manifest -- no eckit-side translation.

    // Grid Definition Template 3.0 (regular lat/lon), 19 entries.
    // ni = points along a parallel (longitudes), nj = points along a
    // meridian (latitudes).
    static std::vector<std::int64_t> build_gdt_3_0(const Grib2Settings& s, std::int64_t ni, std::int64_t nj);

    // Product Definition Template 4.0 (analysis/forecast at a level),
    // 15 entries.
    static std::vector<std::int64_t> build_pdt_4_0(const Grib2Settings& s);

    // Data Representation Template values for the selected DRT.
    // DRT 40 (JPEG2000) -> 7 entries; DRT 42 (AEC/CCSDS) -> 8 entries.
    static std::vector<std::int64_t> build_drs_template(GRIB2_DRT drt, const Grib2Settings& s);

   private:
    // Read all Grib2Settings fields from the configuration, applying
    // defaults for any absent key.
    static Grib2Settings read_settings(const eckit::Configuration& config);

    // State
    bool initialized_ = false;
    Grib2Settings settings_;
    std::string output_path_;
    GRIB2_DRT active_drt_ = GRIB2_DRT::AdaptiveEntropyCoding;
    std::FILE* out_file_ = nullptr;
};

}  // namespace amio::detail

#endif  // AMIO_SRC_DRIVERS_GRIB2_GRIB2_DRIVER_HPP
