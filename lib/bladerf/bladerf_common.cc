/* -*- c++ -*- */
/*
 * Copyright 2013-2017 Nuand LLC
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#include "bladerf_common.h"

/* Defaults for these values. */
static size_t const NUM_BUFFERS = 512;
static size_t const NUM_SAMPLES_PER_BUFFER = (4 * 1024);
static size_t const NUM_TRANSFERS = 32;
static size_t const STREAM_TIMEOUT_MS = 3000;

using namespace boost::assign;

std::mutex bladerf_common::_devs_mutex;
std::list<std::weak_ptr<struct bladerf>> bladerf_common::_devs;

/* name for system-wide gain (which is not its own libbladeRF gain stage) */
static const char *SYSTEM_GAIN_NAME = "system";

/* Determines if bladerf_version is greater or equal to major.minor.patch */
static bool _version_greater_or_equal(const struct bladerf_version *version,
                                      unsigned int major,
                                      unsigned int minor, unsigned int patch)
{
  if (version->major > major) {
    // 2.0.0 > 1.9.9
    return true;
  } else if ((version->major == major) && (version->minor > minor)) {
    // 1.9.9 > 1.8.9
    return true;
  } else if ((version->major == major) &&
             (version->minor == minor) &&
             (version->patch >= patch)) {
    // 1.8.9 > 1.8.8
    return true;
  } else {
    return false;
  }
}

/* Returns TRUE if an expansion board is attached, FALSE otherwise */
static bool _is_xb_attached(bladerf_sptr _dev)
{
  int status;
  bladerf_xb xb = BLADERF_XB_NONE;

  status = bladerf_expansion_get_attached(_dev.get(), &xb);
  if (status != 0) {
    return false;
  }

  return (xb != BLADERF_XB_NONE);
}

/* Gets a value from a const dict */
static std::string const _get(dict_t const &dict, std::string key)
{
  std::string rv("");

  dict_t::const_iterator it = dict.find(key);

  if (it != dict.end()) {
    rv = it->second;
  }

  return rv;
}

static bool _is_tx(bladerf_channel ch)
{
  return (1 == (ch & BLADERF_DIRECTION_MASK));
}

size_t num_streams(bladerf_channel_layout layout)
{
#ifdef BLADERF_COMPATIBILITY
  return 1;
#else

  switch (layout) {
    case BLADERF_RX_X1:
    case BLADERF_TX_X1:
      return 1;
    case BLADERF_RX_X2:
    case BLADERF_TX_X2:
      return 2;
  }

  assert(false);

  return 0;
#endif
}

/******************************************************************************
 * Public methods
 ******************************************************************************/
bladerf_common::bladerf_common() :
  _dev(NULL),
  _pfx("[bladeRF common] "),
  _failures(0),
  _num_buffers(NUM_BUFFERS),
  _samples_per_buffer(NUM_SAMPLES_PER_BUFFER),
  _num_transfers(NUM_TRANSFERS),
  _stream_timeout(STREAM_TIMEOUT_MS),
  _format(BLADERF_FORMAT_SC16_Q11)
{
}

/******************************************************************************
 * Protected methods
 ******************************************************************************/
void bladerf_common::init(dict_t const &dict, bladerf_direction direction)
{
  int status;
  std::string device_name("");
  struct bladerf_version ver;

  BLADERF_DEBUG("entering initialization");

  _pfx = boost::str(boost::format("[bladeRF %s] ")
          % (direction == BLADERF_TX ? "sink" : "source"));

  /* libbladeRF verbosity */
  if (dict.count("verbosity")) {
    set_verbosity(_get(dict, "verbosity"));
  }

  /* Board identifier */
  if (dict.count("bladerf")) {
    std::string const value = _get(dict, "bladerf");
    if (value.length() > 0) {
      if (value.length() <= 2) {
        /* If the value is two digits or less, we'll assume the user is
         * providing an instance number */
        unsigned int device_number = 0;

        try {
          device_number = boost::lexical_cast<unsigned int>(value);
          device_name = boost::str(boost::format("*:instance=%d")
                                   % device_number);
        } catch (std::exception &ex) {
          BLADERF_THROW(boost::str(boost::format("Failed to use '%s' as "
                        "device number: %s") % value % ex.what()));
        }

      } else {
        /* Otherwise, we'll assume it's a serial number. libbladeRF v1.4.1
         * supports matching a subset of a serial number. For earlier versions,
         * we require the entire serial number.
         *
         * libbladeRF is responsible for rejecting bad serial numbers, so we
         * may just pass whatever the user has provided.
         */
        bladerf_version(&ver);
        if (_version_greater_or_equal(&ver, 1, 4, 1) ||
            value.length() == (BLADERF_SERIAL_LENGTH - 1)) {
          device_name = std::string("*:serial=") + value;
        } else {
          BLADERF_THROW(boost::str(boost::format("A full serial number must "
                        "be supplied with libbladeRF %s. libbladeRF >= v1.4.1 "
                        "supports opening a device via a subset of its serial "
                        "#.") % ver.describe));
        }
      }
    }
  }

  /* Open the board! */
  try {
    BLADERF_INFO(boost::str(boost::format("Opening Nuand bladeRF with "
                  "device identifier string '%s'") % device_name));

    _dev = open(device_name);
  } catch (std::exception &ex) {
    BLADERF_THROW(boost::str(boost::format("Failed to open bladeRF device "
                  "'%s': %s") % device_name % ex.what()));
  }

  if (NULL == _dev) {
    BLADERF_THROW(boost::str(boost::format("Failed to get device handle for "
                  "'%s': _dev is NULL") % device_name));
  }

  /* Load a FPGA */
  if (dict.count("fpga")) {
    if (dict.count("fpga-reload") == 0 &&
        bladerf_is_fpga_configured(_dev.get()) == 1) {

      BLADERF_WARNING("FPGA is already loaded. Set fpga-reload=1 to force a "
                      "reload.");
    } else {
      std::string fpga = _get(dict, "fpga");

      BLADERF_INFO("Loading FPGA bitstream from " << fpga);

      status = bladerf_load_fpga(_dev.get(), fpga.c_str());
      if (status != 0) {
        BLADERF_WARNING("Could not load FPGA bitstream: "
                        << bladerf_strerror(status));
      } else {
        BLADERF_INFO("The FPGA bitstream was loaded successfully");
      }
    }
  }

  if (bladerf_is_fpga_configured(_dev.get()) != 1) {
    BLADERF_THROW("The FPGA is not configured! Provide device argument "
                  "fpga=/path/to/the/bitstream.rbf to load it.");
  }

  /* XB-200 Transverter Board */
  if (dict.count("xb200")) {
    status = bladerf_expansion_attach(_dev.get(), BLADERF_XB_200);
    if (status != 0) {
      BLADERF_WARNING("Could not attach XB-200: " << bladerf_strerror(status));
    } else {
      bladerf_xb200_filter filter = BLADERF_XB200_AUTO_1DB;

      if (_get(dict, "xb200") == "custom") {
        filter = BLADERF_XB200_CUSTOM;
      } else if (_get(dict, "xb200") == "50M") {
        filter = BLADERF_XB200_50M;
      } else if (_get(dict, "xb200") == "144M") {
        filter = BLADERF_XB200_144M;
      } else if (_get(dict, "xb200") == "222M") {
        filter = BLADERF_XB200_222M;
      } else if (_get(dict, "xb200") == "auto3db") {
        filter = BLADERF_XB200_AUTO_3DB;
      } else if (_get(dict, "xb200") == "auto") {
        filter = BLADERF_XB200_AUTO_1DB;
      } else {
        filter = BLADERF_XB200_AUTO_1DB;
      }

      status = bladerf_xb200_set_filterbank(_dev.get(), direction, filter);
      if (status != 0) {
        BLADERF_WARNING("Could not set XB-200 filter: "
                        << bladerf_strerror(status));
      }
    }
  }

  /* Show some info about the device we've opened */
  print_device_info();

  if (dict.count("tamer")) {
    set_clock_source(_get(dict, "tamer"));
    BLADERF_INFO(boost::str(boost::format("Tamer mode set to '%s'")
                  % get_clock_source()));
  }

  if (dict.count("smb")) {
    set_smb_frequency(boost::lexical_cast<double>(_get(dict, "smb")));
    BLADERF_INFO(boost::str(boost::format("SMB frequency set to %f Hz")
                  % get_smb_frequency()));
  }

  /* Initialize buffer and sample configuration */
  if (dict.count("buffers")) {
    _num_buffers = boost::lexical_cast<size_t>(_get(dict, "buffers"));
  }

  if (dict.count("buflen")) {
    _samples_per_buffer = boost::lexical_cast<size_t>(_get(dict, "buflen"));
  }

  if (dict.count("transfers")) {
    _num_transfers = boost::lexical_cast<size_t>(_get(dict, "transfers"));
  }

  if (dict.count("stream_timeout")) {
    _stream_timeout = boost::lexical_cast<unsigned int>(_get(dict, "stream_timeout"));
  } else if (dict.count("stream_timeout_ms")) {
    // reverse compatibility
    _stream_timeout = boost::lexical_cast<unsigned int>(_get(dict, "stream_timeout_ms"));
  }

  if (dict.count("enable_metadata") > 0) {
    _format = BLADERF_FORMAT_SC16_Q11_META;
  }

  /* Require value to be >= 2 so we can ensure we have twice as many
   * buffers as transfers */
  if (_num_buffers <= 1) {
    _num_buffers = NUM_BUFFERS;
  }

  if (0 == _samples_per_buffer) {
    _samples_per_buffer = NUM_SAMPLES_PER_BUFFER;
  } else {
    if ((_samples_per_buffer < 1024) || (_samples_per_buffer % 1024 != 0)) {
      BLADERF_WARNING(boost::str(boost::format("Invalid \"buflen\" value "
                      "(%d). A multiple of 1024 is required. Defaulting "
                      "to %d")
                      % _samples_per_buffer % NUM_SAMPLES_PER_BUFFER));
      _samples_per_buffer = NUM_SAMPLES_PER_BUFFER;
    }
  }

  /* If the user hasn't specified the desired number of transfers, set it to
   * at least num_buffers/2 */
  if (0 == _num_transfers) {
    _num_transfers = std::min(NUM_TRANSFERS, _num_buffers / 2);
  } else if (_num_transfers >= _num_buffers) {
    _num_transfers = std::min(NUM_TRANSFERS, _num_buffers / 2);
    BLADERF_WARNING(boost::str(boost::format("Clamping \"transfers\" to %d. "
                    "Try using a smaller \"transfers\" value if timeouts "
                    "occur.") % _num_transfers));
  }

  BLADERF_INFO(boost::str(boost::format("Buffers: %d, samples per buffer: "
                "%d, active transfers: %d")
                % _num_buffers
                % _samples_per_buffer
                % _num_transfers));
}

std::vector<std::string> bladerf_common::devices()
{
  struct bladerf_devinfo *devices;
  ssize_t n_devices;
  std::vector<std::string> ret;

  n_devices = bladerf_get_device_list(&devices);

  if (n_devices > 0) {
    for (ssize_t i = 0; i < n_devices; i++) {
      std::string serial(devices[i].serial);
      std::string devstr;

      if (serial.length() == 32) {
        serial.replace(4, 24, "...");
      }

      devstr = boost::str(boost::format("bladerf=%s,label='Nuand bladeRF%s%s'")
                % devices[i].instance
                % (serial.length() > 0 ? " SN " : "")
                % serial);

      ret.push_back(devstr);
    }

    bladerf_free_device_list(devices);
  }

  return ret;
}

bladerf_board_type bladerf_common::get_board_type()
{
  if (NULL == _dev || NULL == _dev.get()) {
    BLADERF_WARNING("no bladeRF device is open");
    return BOARD_TYPE_NONE;
  }

  std::string boardname = std::string(bladerf_get_board_name(_dev.get()));

  if (boardname == "bladerf1") {
    return BOARD_TYPE_BLADERF_1;
  }

  if (boardname == "bladerf2") {
    return BOARD_TYPE_BLADERF_2;
  }

  BLADERF_WARNING(boost::str(boost::format("model '%s' is not recognized")
                  % boardname));

  return BOARD_TYPE_UNKNOWN;
}

size_t bladerf_common::get_max_channels(bladerf_direction direction)
{
#ifdef BLADERF_COMPATIBILITY
  return 1;
#else
  return bladerf_get_channel_count(_dev.get(), direction);
#endif
}

void bladerf_common::set_channel_enable(bladerf_channel ch, bool enable)
{
  _enables[ch] = enable;
}

bool bladerf_common::get_channel_enable(bladerf_channel ch)
{
  return _enables[ch];
}

void bladerf_common::set_verbosity(std::string const &verbosity)
{
  bladerf_log_level l;

  if (verbosity == "verbose") {
    l = BLADERF_LOG_LEVEL_VERBOSE;
  } else if (verbosity == "debug") {
    l = BLADERF_LOG_LEVEL_DEBUG;
  } else if (verbosity == "info") {
    l = BLADERF_LOG_LEVEL_INFO;
  } else if (verbosity == "warning") {
    l = BLADERF_LOG_LEVEL_WARNING;
  } else if (verbosity == "error") {
    l = BLADERF_LOG_LEVEL_ERROR;
  } else if (verbosity == "critical") {
    l = BLADERF_LOG_LEVEL_CRITICAL;
  } else if (verbosity == "silent") {
    l = BLADERF_LOG_LEVEL_SILENT;
  } else {
    BLADERF_THROW(boost::str(boost::format("Invalid log level: %s")
                  % verbosity));
  }

  bladerf_log_set_verbosity(l);
}

bladerf_channel bladerf_common::str2channel(std::string const &ch)
{
  std::string prefix, numstr;
  unsigned int numint;

  /* We expect strings like "RX1" or "TX2" */
  if (ch.length() < 3) {
    /* It's too short */
    return BLADERF_CHANNEL_INVALID;
  }

  prefix = ch.substr(0,2);
  numstr = ch.substr(2,std::string::npos);
  numint = boost::lexical_cast<unsigned int>(numstr) - 1;

  if (prefix == "RX") {
    return BLADERF_CHANNEL_RX(numint);
  }

  if (prefix == "TX") {
    return BLADERF_CHANNEL_TX(numint);
  }

  return BLADERF_CHANNEL_INVALID;
}

std::string bladerf_common::channel2str(bladerf_channel ch)
{
  if (ch == BLADERF_CHANNEL_INVALID) {
    return "OFF";
  }

  return boost::str(boost::format("%s%d")
          % (_is_tx(ch) ? "TX" : "RX")
          % (channel2rfport(ch) + 1));
}

int bladerf_common::channel2rfport(bladerf_channel ch)
{
  return (ch >> 1);
}

bladerf_channel bladerf_common::chan2channel(bladerf_direction direction,
                                             size_t chan)
{
  for (bladerf_channel_map::value_type &i : _chanmap) {
    bladerf_channel ch = i.first;
    if (
        (i.second == (int)chan) && (
         (direction == BLADERF_TX && _is_tx(ch)) ||
         (direction == BLADERF_RX && !_is_tx(ch))
        )
       ) {
      return i.first;
    }
  }

  return BLADERF_CHANNEL_INVALID;
}

osmosdr::meta_range_t bladerf_common::sample_rates(bladerf_channel ch)
{
  osmosdr::meta_range_t sample_rates;

#ifdef BLADERF_COMPATIBILITY
  /* assuming the same for RX & TX */
  sample_rates += osmosdr::range_t( 160e3, 200e3, 40e3 );
  sample_rates += osmosdr::range_t( 300e3, 900e3, 100e3 );
  sample_rates += osmosdr::range_t( 1e6, 40e6, 1e6 );
#else

  int status;
  const bladerf_range *brf_sample_rates;

  status = bladerf_get_sample_rate_range(_dev.get(), ch, &brf_sample_rates);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "bladerf_get_sample_rate_range failed");
  }

  /* Suggest a variety of sample rates */
  sample_rates += osmosdr::range_t(brf_sample_rates->min,
                                   brf_sample_rates->max / 4.0,
                                   brf_sample_rates->max / 16.0);
  sample_rates += osmosdr::range_t(brf_sample_rates->max / 4.0,
                                   brf_sample_rates->max / 2.0,
                                   brf_sample_rates->max / 8.0);
  sample_rates += osmosdr::range_t(brf_sample_rates->max / 2.0,
                                   brf_sample_rates->max,
                                   brf_sample_rates->max / 4.0);
#endif

  return sample_rates;
}

double bladerf_common::set_sample_rate(double rate, bladerf_channel ch)
{
  int status;
  struct bladerf_rational_rate rational_rate, actual;

  rational_rate.integer = static_cast<uint32_t>(rate);
  rational_rate.den = 10000;
  rational_rate.num = (rate - rational_rate.integer) * rational_rate.den;

  status = bladerf_set_rational_sample_rate(_dev.get(), ch,
                                            &rational_rate, &actual);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to set sample rate");
  }

  return actual.integer + (actual.num / static_cast<double>(actual.den));
}

double bladerf_common::get_sample_rate(bladerf_channel ch)
{
  int status;
  struct bladerf_rational_rate rate;

  status = bladerf_get_rational_sample_rate(_dev.get(), ch, &rate);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to get sample rate");
  }

  return rate.integer + rate.num / static_cast<double>(rate.den);
}

osmosdr::freq_range_t bladerf_common::freq_range(bladerf_channel ch)
{
#ifdef BLADERF_COMPATIBILITY
  return osmosdr::freq_range_t( _is_xb_attached(_dev) ? 0 : 280e6,
                                BLADERF_FREQUENCY_MAX );
#else

  int status;
  const struct bladerf_range *range;

  status = bladerf_get_frequency_range(_dev.get(), ch, &range);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "bladerf_get_frequency_range failed");
  };

  return osmosdr::freq_range_t(static_cast<double>(range->min),
                               static_cast<double>(range->max),
                               static_cast<double>(range->step));
#endif
}

double bladerf_common::set_center_freq(double freq, bladerf_channel ch)
{
  int status;
  uint64_t freqint = static_cast<uint64_t>(freq + 0.5);

  /* Check frequency range */
  if (freqint < freq_range(ch).start() || freqint > freq_range(ch).stop()) {
    BLADERF_WARNING(boost::str(boost::format("Frequency %d Hz is outside "
                    "range, ignoring") % freqint));
  } else {
    status = bladerf_set_frequency(_dev.get(), ch, freqint);
    if (status != 0) {
      BLADERF_THROW_STATUS(status, boost::str(boost::format("Failed to set center "
                    "frequency to %d Hz") % freqint));
    }
  }

  return get_center_freq(ch);
}

double bladerf_common::get_center_freq(bladerf_channel ch)
{
  int status;
  uint64_t freq;

  status = bladerf_get_frequency(_dev.get(), ch, &freq);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to get center frequency");
  }

  return static_cast<double>(freq);
}

osmosdr::freq_range_t bladerf_common::filter_bandwidths(bladerf_channel ch)
{
  osmosdr::freq_range_t bandwidths;

#ifdef BLADERF_COMPATIBILITY
  std::vector<double> half_bandwidths; /* in MHz */
  half_bandwidths += \
      0.75, 0.875, 1.25, 1.375, 1.5, 1.92, 2.5,
      2.75, 3, 3.5, 4.375, 5, 6, 7, 10, 14;

  for (double half_bw : half_bandwidths)
    bandwidths += osmosdr::range_t( half_bw * 2e6 );
#else

  int status;
  const bladerf_range *range;

  status = bladerf_get_bandwidth_range(_dev.get(), ch, &range);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "bladerf_get_bandwidth_range failed");
  }

  bandwidths += osmosdr::range_t(range->min, range->max, range->step);
#endif

  return bandwidths;
}

double bladerf_common::set_bandwidth(double bandwidth, bladerf_channel ch)
{
  int status;
  uint32_t bwint;

  if (bandwidth == 0.0) {
    /* bandwidth of 0 means automatic filter selection */
    /* select narrower filters to prevent aliasing */
    bandwidth = get_sample_rate(ch) * 0.75;
  }

  bwint = static_cast<uint32_t>(bandwidth + 0.5);

  status = bladerf_set_bandwidth(_dev.get(), ch, bwint, NULL);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "could not set bandwidth");
  }

  return get_bandwidth(ch);
}

double bladerf_common::get_bandwidth(bladerf_channel ch)
{
  int status;
  uint32_t bandwidth;

  status = bladerf_get_bandwidth(_dev.get(), ch, &bandwidth);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "could not get bandwidth");
  }

  return static_cast<double>(bandwidth);
}

std::vector<std::string> bladerf_common::get_gain_names(bladerf_channel ch)
{
  std::vector<std::string> names;

#ifdef BLADERF_COMPATIBILITY
  names += "LNA", "VGA1", "VGA2";
#else

  const size_t max_count = 16;
  char *gain_names[max_count];
  int count;
  names += SYSTEM_GAIN_NAME;

  count = bladerf_get_gain_stages(_dev.get(), ch,
                                  reinterpret_cast<const char **>(&gain_names),
                                  max_count);
  if (count < 0) {
    BLADERF_THROW_STATUS(count, "Failed to enumerate gain stages");
  }

  for (int i = 0; i < count; ++i) {
    char *tmp = gain_names[i];
    printf("FOUND %s\n", tmp);
    names += std::string(tmp);
  };
#endif

  return names;
}

osmosdr::gain_range_t bladerf_common::get_gain_range(bladerf_channel ch)
{
  /* This is an overall system gain range. */
  return get_gain_range(SYSTEM_GAIN_NAME, ch);
}

osmosdr::gain_range_t bladerf_common::get_gain_range(std::string const &name,
                                                     bladerf_channel ch)
{
#ifdef BLADERF_COMPATIBILITY
  if( name == "LNA" ) {
    return osmosdr::gain_range_t( 0, 6, 3 );
  } else if( name == "VGA1" ) {
    return osmosdr::gain_range_t( 5, 30, 1 );
  } else if( name == "VGA2" ) {
    return osmosdr::gain_range_t( 0, 30, 3 );
  } else {
    BLADERF_THROW_STATUS(BLADERF_ERR_UNSUPPORTED, boost::str(boost::format(
                         "Failed to get gain range for stage '%s'") % name));
  }
#else

  int status;
  const bladerf_range *range;

  if (name == SYSTEM_GAIN_NAME) {
    status = bladerf_get_gain_range(_dev.get(), ch, &range);
  } else {
    status = bladerf_get_gain_stage_range(_dev.get(), ch, name.c_str(), &range);
  }

  if (status != 0) {
    BLADERF_THROW_STATUS(status, boost::str(boost::format("Failed to get gain "
                         "range for stage '%s'") % name));
  }

  return osmosdr::gain_range_t(range->min, range->max, range->step);
#endif
}

bool bladerf_common::set_gain_mode(bool automatic, bladerf_channel ch,
                                   bladerf_gain_mode agc_mode)
{
  int status;
  bladerf_gain_mode mode = automatic ? agc_mode : BLADERF_GAIN_MGC;

  status = bladerf_set_gain_mode(_dev.get(), ch, mode);

  if (status != 0) {
    BLADERF_THROW_STATUS(status, boost::str(boost::format("Setting gain mode "
                         "to '%s' failed")
                         % (automatic ? "automatic" : "manual")));
  }

  return get_gain_mode(ch);
}

bool bladerf_common::get_gain_mode(bladerf_channel ch)
{
  int status;
  bladerf_gain_mode gainmode = BLADERF_GAIN_DEFAULT;

  status = bladerf_get_gain_mode(_dev.get(), ch, &gainmode);

  if (status != 0) {
    BLADERF_WARN_STATUS(status, "Failed to get gain mode");
  }

  return (gainmode != BLADERF_GAIN_MGC);
}

double bladerf_common::set_gain(double gain, bladerf_channel ch)
{
  return set_gain(gain, SYSTEM_GAIN_NAME, ch);
}

double bladerf_common::set_gain(double gain,
                                std::string const &name,
                                bladerf_channel ch)
{
  int status;

#ifdef BLADERF_COMPATIBILITY
  if( name == "LNA" ) {
    bladerf_lna_gain g;

    if ( gain >= 6.0f )
      g = BLADERF_LNA_GAIN_MAX;
    else if ( gain >= 3.0f )
      g = BLADERF_LNA_GAIN_MID;
    else /* gain < 3.0f */
      g = BLADERF_LNA_GAIN_BYPASS;

    status = bladerf_set_lna_gain( _dev.get(), g );
  } else if( name == "VGA1" ) {
    status = bladerf_set_rxvga1( _dev.get(), (int)gain );
  } else if( name == "VGA2" ) {
    status = bladerf_set_rxvga2( _dev.get(), (int)gain );
  } else {
    status = BLADERF_ERR_UNSUPPORTED;
  }
#else

  if (name == SYSTEM_GAIN_NAME) {
    status = bladerf_set_gain(_dev.get(), ch, static_cast<int>(gain));
  } else {
    status = bladerf_set_gain_stage(_dev.get(), ch, name.c_str(),
                                    static_cast<int>(gain));
  }
#endif

  /* Check for errors */
  if (BLADERF_ERR_UNSUPPORTED == status) {
    // unsupported, but not worth crashing out
    BLADERF_WARNING(boost::str(boost::format("Gain stage '%s' not supported "
                    "by device") % name));
  } else if (status != 0) {
    BLADERF_THROW_STATUS(status, boost::str(boost::format("Failed to set "
                         "gain for stage '%s'") % name));
  }

  return get_gain(name, ch);
}

double bladerf_common::get_gain(bladerf_channel ch)
{
  return get_gain(SYSTEM_GAIN_NAME, ch);
}

double bladerf_common::get_gain(std::string const &name, bladerf_channel ch)
{
  int status;
  int g = 0;

#ifdef BLADERF_COMPATIBILITY
  if( name == "LNA" ) {
    bladerf_lna_gain lna_g;
    status = bladerf_get_lna_gain( _dev.get(), &lna_g );
    g = lna_g == BLADERF_LNA_GAIN_BYPASS ? 0 : lna_g == BLADERF_LNA_GAIN_MID ? 3 : 6;
  } else if( name == "VGA1" ) {
    status = bladerf_get_rxvga1( _dev.get(), &g );
  } else if( name == "VGA2" ) {
    status = bladerf_get_rxvga2( _dev.get(), &g );
  } else {
    status = BLADERF_ERR_UNSUPPORTED;
  }
#else

  if (name == SYSTEM_GAIN_NAME) {
    status = bladerf_get_gain(_dev.get(), ch, &g);
  } else {
    status = bladerf_get_gain_stage(_dev.get(), ch, name.c_str(), &g);
  }
#endif

  /* Check for errors */
  if (status != 0) {
    BLADERF_WARN_STATUS(status, boost::str(boost::format("Could not get gain "
                         "for stage '%s'") % name));
  }

  return static_cast<double>(g);
}

std::vector<std::string> bladerf_common::get_antennas(bladerf_direction dir)
{
  std::vector<std::string> antennas;

  for (size_t i = 0; i < get_max_channels(dir); ++i) {
    switch (dir) {
      case BLADERF_RX:
        antennas += channel2str(BLADERF_CHANNEL_RX(i));
        break;
      case BLADERF_TX:
        antennas += channel2str(BLADERF_CHANNEL_TX(i));
        break;
      default:
        break;
    }
  }

  return antennas;
}

bool bladerf_common::set_antenna(bladerf_direction dir,
                                 size_t chan,
                                 const std::string &antenna)
{
  if (!is_antenna_valid(dir, antenna)) {
    BLADERF_THROW("Invalid antenna: " + antenna);
  }

  // This port's old antenna
  bladerf_channel old_channel = chan2channel(dir, chan);
  // This port's new antenna
  bladerf_channel new_channel = str2channel(antenna);
  // The new antenna's old port
  int old_chan = _chanmap[new_channel];

  if (old_channel != new_channel || old_chan != (int)chan) {
    // Disable the old antenna, if it's not going to be used
    if (old_chan == -1) {
      set_channel_enable(old_channel, false);
    }

    // Swap antennas
    _chanmap[old_channel] = old_chan;
    _chanmap[new_channel] = chan;

    // Enable the new antenna
    set_channel_enable(new_channel, true);
  }

  return true;
}

int bladerf_common::set_dc_offset(std::complex<double> const &offset,
                                  bladerf_channel ch)
{
  int ret = 0;
  int16_t val_i, val_q;

  val_i = static_cast<int16_t>(offset.real() * DCOFF_SCALE);
  val_q = static_cast<int16_t>(offset.imag() * DCOFF_SCALE);

  ret = bladerf_set_correction(_dev.get(), ch,
                               BLADERF_CORR_LMS_DCOFF_I, val_i);
  ret |= bladerf_set_correction(_dev.get(), ch,
                                BLADERF_CORR_LMS_DCOFF_Q, val_q);

  return ret;
}

int bladerf_common::set_iq_balance(std::complex<double> const &balance,
                                   bladerf_channel ch)
{
  int ret = 0;
  int16_t val_gain, val_phase;

  val_gain = static_cast<int16_t>(balance.real() * GAIN_SCALE);
  val_phase = static_cast<int16_t>(balance.imag() * PHASE_SCALE);

  ret = bladerf_set_correction(_dev.get(), ch,
                               BLADERF_CORR_FPGA_GAIN, val_gain);
  ret |= bladerf_set_correction(_dev.get(), ch,
                                BLADERF_CORR_FPGA_PHASE, val_phase);

  return ret;
}

std::vector<std::string> bladerf_common::get_clock_sources(size_t mboard)
{
  std::vector<std::string> sources;

  // assumes zero-based 1:1 mapping
  sources.push_back("internal");        // BLADERF_VCTCXO_TAMER_DISABLED
  sources.push_back("external_1pps");   // BLADERF_VCTCXO_TAMER_1_PPS
  sources.push_back("external");        // BLADERF_VCTCXO_TAMER_10_MHZ

  return sources;
}

void bladerf_common::set_clock_source(std::string const &source,
                                      size_t mboard)
{
  int status;
  bladerf_vctcxo_tamer_mode tamer_mode;
  std::vector<std::string> clock_sources;
  int index;

  tamer_mode = BLADERF_VCTCXO_TAMER_DISABLED;
  clock_sources = get_clock_sources(mboard);
  index = std::find(clock_sources.begin(), clock_sources.end(), source) - clock_sources.begin();

  if (index < static_cast<int>(clock_sources.size())) {
    tamer_mode = static_cast<bladerf_vctcxo_tamer_mode>(index);
  }

  status = bladerf_set_vctcxo_tamer_mode(_dev.get(), tamer_mode);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to set VCTCXO tamer mode");
  }
}

std::string bladerf_common::get_clock_source(size_t mboard)
{
  int status;
  bladerf_vctcxo_tamer_mode tamer_mode;
  std::vector<std::string> clock_sources;

  tamer_mode = BLADERF_VCTCXO_TAMER_INVALID;

  status = bladerf_get_vctcxo_tamer_mode(_dev.get(), &tamer_mode);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to get VCTCXO tamer mode");
  }

  clock_sources = get_clock_sources(mboard);

  return clock_sources.at(tamer_mode);
}

void bladerf_common::set_smb_frequency(double frequency)
{
  int status;
  uint32_t freqint = static_cast<uint32_t>(frequency + 0.5);
  uint32_t actual_frequency = freqint;

  if (_is_xb_attached(_dev)) {
    BLADERF_WARNING("Cannot use SMB port when expansion board is attached");
    return;
  }

  status = bladerf_set_smb_frequency(_dev.get(),
                                     freqint,
                                     &actual_frequency);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to set SMB frequency");
  }

  if (freqint != actual_frequency) {
    BLADERF_WARNING(boost::str(boost::format("Wanted SMB frequency %f (%d) "
                    "Hz, actual frequency is %d Hz")
                    % frequency % freqint % actual_frequency));
  }
}

double bladerf_common::get_smb_frequency()
{
  int status;
  unsigned int actual_frequency;

  if (_is_xb_attached(_dev)) {
    BLADERF_WARNING("Cannot use SMB port when expansion board is attached");
    return 0.0;
  }

  status = bladerf_get_smb_frequency(_dev.get(), &actual_frequency);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to get SMB frequency");
  }

  return static_cast<double>(actual_frequency);
}

/******************************************************************************
 * Private methods
 ******************************************************************************/
bladerf_sptr bladerf_common::open(std::string const &device_name)
{
  int status;
  struct bladerf *raw_dev = NULL;
  struct bladerf_devinfo devinfo;

  std::lock_guard<std::mutex> lock(_devs_mutex);

  /* Initialize the information used to identify the desired device
   * to all wildcard (i.e., "any device") values */
  bladerf_init_devinfo(&devinfo);

  /* Populate the devinfo structure from device_name */
  status = bladerf_get_devinfo_from_str(device_name.c_str(), &devinfo);
  if (status < 0) {
    BLADERF_THROW_STATUS(status, boost::str(boost::format("Failed to get "
                         "devinfo for '%s'") % device_name));
  }

  /* Do we already have this device open? */
  bladerf_sptr cached_dev = get_cached_device(devinfo);

  if (cached_dev) {
    return cached_dev;
  }

  /* Open the device. */
  status = bladerf_open_with_devinfo(&raw_dev, &devinfo);
  if (status < 0) {
    BLADERF_THROW_STATUS(status, boost::str(boost::format("Failed to open "
                         "device for '%s'") % device_name));
  }

  /* Add the device handle to our cache */
  bladerf_sptr dev = bladerf_sptr(raw_dev, bladerf_common::close);

  _devs.push_back(static_cast<std::weak_ptr<struct bladerf>>(dev));

  return dev;
}

void bladerf_common::close(void *dev)
{
  std::lock_guard<std::mutex> lock(_devs_mutex);
  std::list<std::weak_ptr<struct bladerf>>::iterator it(_devs.begin());

  /* Prune expired entries from device cache */
  while (it != _devs.end()) {
    if ((*it).expired()) {
      it = _devs.erase(it);
    } else {
      ++it;
    }
  }

  bladerf_close(static_cast<struct bladerf *>(dev));
}

bladerf_sptr bladerf_common::get_cached_device(struct bladerf_devinfo devinfo)
{
  /* Lock to _devs must be aquired by caller */
  int status;
  struct bladerf_devinfo other_devinfo;

  for (std::weak_ptr<struct bladerf> dev : _devs) {
    status = bladerf_get_devinfo(bladerf_sptr(dev).get(), &other_devinfo);
    if (status < 0) {
      BLADERF_THROW_STATUS(status, "Failed to get devinfo for cached device");
    }

    if (bladerf_devinfo_matches(&devinfo, &other_devinfo)) {
      return bladerf_sptr(dev);
    }
  }

  return bladerf_sptr();
}

void bladerf_common::print_device_info()
{
  char serial[BLADERF_SERIAL_LENGTH];
  struct bladerf_version ver;

  std::cout << _pfx << "Device: ";

  switch (get_board_type()) {
    case BOARD_TYPE_BLADERF_1:
      std::cout << "Nuand bladeRF";
      break;
    case BOARD_TYPE_BLADERF_2:
      std::cout << "Nuand bladeRF 2.0";
      break;
    default:
      std::cout << "Unknown Device";
      break;
  }

  if (bladerf_get_serial(_dev.get(), serial) == 0) {
    std::string strser(serial);

    if (strser.length() == 32) {
      strser.replace(4, 24, "...");
    }

    std::cout << " Serial # " << strser;
  } else {
    std::cout << " Serial # UNKNOWN";
  }

  if (bladerf_fw_version(_dev.get(), &ver) == 0) {
    std::cout << " FW v" << ver.major << "." << ver.minor << "." << ver.patch;
  } else {
    std::cout << " FW version UNKNOWN";
  }

  if (bladerf_fpga_version(_dev.get(), &ver) == 0) {
    std::cout << " FPGA v" << ver.major << "." << ver.minor << "." << ver.patch;
  } else {
    std::cout << " FPGA version UNKNOWN";
  }

  std::cout << std::endl;
}

bool bladerf_common::is_antenna_valid(bladerf_direction dir,
                                      const std::string &antenna)
{
  for (std::string ant : get_antennas(dir)) {
    if (antenna == ant) {
      return true;
    }
  }

  return false;
}
