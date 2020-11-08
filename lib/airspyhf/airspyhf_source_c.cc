/* -*- c++ -*- */
/*
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

#include <stdexcept>
#include <iostream>
#include <string>
#include <algorithm>
#include <osmosdr/ranges.h>
#include <gnuradio/io_signature.h>
#include "arg_helpers.h"

#include "airspyhf_source_c.h"

#define MAX_DEVICES  32 // arbitrary number

#define AIRSPYHF_INFO(message) \
    { \
    std::cout << "[AirspyHF] " << __FUNCTION__ << ": " << message << std::endl; \
    }

#define AIRSPYHF_WARNING(message) \
    { \
    std::cerr << "[AirspyHF] " << __FUNCTION__ << ": " << message << std::endl; \
    }

airspyhf_source_c_sptr make_airspyhf_source_c (const std::string & args)
{
    return gnuradio::get_initial_sptr(new airspyhf_source_c (args));
}

/*
 * The private constructor
 */
airspyhf_source_c::airspyhf_source_c (const std::string &args)
: gr::sync_block ("airspyhf_source_c",
                  gr::io_signature::make(0, 0, 0),
                  gr::io_signature::make(1, 1, sizeof (gr_complex))),
_sample_rate(0),
_center_freq(0),
_freq_corr(0),
_dev(nullptr),
_lna(1),
_att(0),
_agc(1),
_stream_buff(nullptr)
{
    int ret;
    
    // TODO: open by serial
    dict_t args_dict = params_to_dict(args);
    
    if (args_dict.count("serial") != 0) {
        AIRSPYHF_INFO("using serial" << args_dict.at("serial"));

        uint64_t serial;
        try {
            serial = std::stoull(args_dict.at("serial"), nullptr, 16);
        } catch (const std::invalid_argument &) {
            throw std::runtime_error("serial is not a hex number");
        } catch (const std::out_of_range &) {
            throw std::runtime_error("serial value of out range");
        }
        
        ret = airspyhf_open_sn(&_dev, serial);
        if(ret != AIRSPYHF_SUCCESS) {
            throw std::runtime_error("airspyhf_open");
        }
    } else {
        // Open without serial
        AIRSPYHF_INFO("not using serial");
        ret = airspyhf_open(&_dev);
        if(ret != AIRSPYHF_SUCCESS) {
            throw std::runtime_error("airspyhf_open");
        }
    }

    // At least this size in our work function
    _airspyhf_output_size = airspyhf_get_output_size(_dev);
    set_output_multiple(_airspyhf_output_size);
    
    uint32_t num_rates;
    airspyhf_get_samplerates(_dev, &num_rates, 0);
    _samplerates.resize(num_rates);
    // Get supported rates
    ret = airspyhf_get_samplerates(_dev, _samplerates.data(), num_rates);
    assert(ret == AIRSPYHF_SUCCESS);
    
    airspyhf_lib_version_t lib_version;
    airspyhf_read_partid_serialno_t partid_serialno;
    airspyhf_lib_version(&lib_version);
    ret = airspyhf_board_partid_serialno_read(_dev, &partid_serialno);
    assert(ret == AIRSPYHF_SUCCESS);
    
    AIRSPYHF_INFO("libairspyhf "
                  << lib_version.major_version << "."
                  << lib_version.minor_version << "."
                  << lib_version.revision);
    
    airspyhf_set_lib_dsp(_dev, true);

    airspyhf_set_hf_agc(_dev, _agc);
    airspyhf_set_hf_agc_threshold(_dev, 1); // 1 = high
    airspyhf_set_hf_lna(_dev, _lna);
    airspyhf_set_hf_att(_dev, _att);
    
    set_center_freq(14e6);
    set_sample_rate(768e3);
}

/*
 * Our virtual destructor.
 */
airspyhf_source_c::~airspyhf_source_c ()
{
    airspyhf_close(_dev);
}

// C trampoline function
int airspyhf_source_c::_airspyhf_rx_callback(airspyhf_transfer_t *transfer)
{
    airspyhf_source_c *obj = (airspyhf_source_c *)transfer->ctx;
    return obj->airspyhf_rx_callback(transfer);
}

int airspyhf_source_c::airspyhf_rx_callback(airspyhf_transfer_t *t)
{
    // Take stream lock
    std::unique_lock<std::mutex> lock(_stream_mutex);
    
    while (_stream_buff == nullptr && airspyhf_is_streaming(_dev))
        _stream_cond.wait(lock);
    
    if (t->dropped_samples) {
        AIRSPYHF_WARNING("dropped_samples: " << t->dropped_samples);
    }
    // Copy to _stream_buff
    if (_stream_buff != nullptr) {
        std::memcpy(_stream_buff, t->samples, sizeof(gr_complex) * _airspyhf_output_size);
    }
    
    _stream_buff = nullptr;
    _callback_done_cond.notify_one();
    
    // 0 == success
    return 0;
}

bool airspyhf_source_c::start()
{
    assert(_dev != nullptr);
    int ret = airspyhf_start(_dev, _airspyhf_rx_callback, (void *)this);
    assert(ret == AIRSPYHF_SUCCESS);
    
    AIRSPYHF_INFO("start");
    
    return true;
}

bool airspyhf_source_c::stop()
{
    // Take stream lock
    std::unique_lock<std::mutex> lock(_stream_mutex);
    int ret = airspyhf_stop(_dev);
    assert(ret == AIRSPYHF_SUCCESS);
    AIRSPYHF_INFO("stop");

    return true;
}

int airspyhf_source_c::work(int noutput_items,
                            gr_vector_const_void_star &input_items,
                            gr_vector_void_star &output_items )
{
    // Take stream lock
    std::unique_lock<std::mutex> lock(_stream_mutex);
    
    if(!airspyhf_is_streaming(_dev)) {
        // strictly speaking need to implement timeout
        return WORK_DONE;
    } else if (noutput_items < _airspyhf_output_size) {
        // wait until we get called with more
        return 0;
    }

    _stream_buff = output_items[0];
    // Notify callback that the buffer is ready for samples
    _stream_cond.notify_one();
    // Wait for callback to write samples to buffer
    while ((_stream_buff != nullptr) &&
           airspyhf_is_streaming(_dev)) _callback_done_cond.wait(lock);

    return _airspyhf_output_size;
}

std::vector<std::string> airspyhf_source_c::get_devices()
{
    std::vector<std::string> devices;
    std::string label;
    
    std::vector<uint64_t> serials(MAX_DEVICES);
    int count = airspyhf_list_devices(serials.data(), MAX_DEVICES);
    serials.resize(count);
    
    for(std::vector<uint64_t>::size_type i = 0; i != serials.size(); i++) {
        std::stringstream args;
        args << "airspyhf=" << i << ",";
        args << "label='AirspyHF'" << ",";
        args << "serial=" << std::hex << serials[i];
        devices.push_back(args.str());
        
        std::cout << args.str() << std::endl;
    }
    
    return devices;
}

size_t airspyhf_source_c::get_num_channels()
{
    // This is fixed
    return 1;
}

osmosdr::meta_range_t airspyhf_source_c::get_sample_rates()
{
    osmosdr::meta_range_t range;
    
    for (size_t i = 0; i < _samplerates.size(); i++) {
        range.push_back(osmosdr::range_t(_samplerates[i]));
    }
    
    return range;
}

double airspyhf_source_c::set_sample_rate( double rate )
{
    int ret;
    int is_low_if;
    
    ret = airspyhf_set_samplerate(_dev, (uint32_t) rate);
    if (ret == AIRSPYHF_SUCCESS) {
        _sample_rate = rate;
        is_low_if = airspyhf_is_low_if(_dev);
        AIRSPYHF_INFO("samplerate: " << std::to_string(rate));
        AIRSPYHF_INFO("is_low_if: " << std::to_string(is_low_if));
    }
    return _sample_rate;
}

double airspyhf_source_c::get_sample_rate()
{
    return _sample_rate;
}

osmosdr::freq_range_t airspyhf_source_c::get_freq_range( size_t chan )
{
    return osmosdr::freq_range_t(9e3, 260.0e6);
}

double airspyhf_source_c::set_center_freq(double freq, size_t chan)
{
    int ret;
    assert(_dev != nullptr);
    ret = airspyhf_set_freq(_dev, freq);
    if (ret == AIRSPYHF_SUCCESS) {
        _center_freq = freq;
    } else {
        AIRSPYHF_WARNING("set_center_freq failed")
    }
    
    return _center_freq;
}

double airspyhf_source_c::get_center_freq( size_t chan )
{
    return _center_freq;
}

double airspyhf_source_c::set_freq_corr(double ppm, size_t chan )
{
    int ret;
    int32_t ppb = (int32_t)(ppm * 1.0e3);
    assert(_dev != nullptr);
    
    ret = airspyhf_set_calibration(_dev, ppb);
    if (ret == AIRSPYHF_SUCCESS) {
        _freq_corr = ppm;
    } else {
        AIRSPYHF_WARNING("set_freq_corr failed")
    }
    
    return ppm;
}

double airspyhf_source_c::get_freq_corr(size_t chan)
{
    int ret;
    int32_t ppb = 0;
    assert(chan == 0);
    ret = airspyhf_get_calibration(_dev, &ppb);
    assert(ret == AIRSPYHF_SUCCESS);
    return ppb / 1.0e3;
}

std::vector<std::string> airspyhf_source_c::get_gain_names(size_t chan)
{
    assert(chan == 0);
    std::vector<std::string> gains;
    gains.push_back("ATT");
    gains.push_back("LNA");
    return gains;
}

osmosdr::gain_range_t airspyhf_source_c::get_gain_range(size_t chan)
{
    return get_gain_range("ATT", chan);
}

osmosdr::gain_range_t airspyhf_source_c::get_gain_range(const std::string &name, size_t chan)
{
    assert(chan == 0);
    
    // TODO: enable AGC somewhere
    
    if (name == "ATT") {
        // Possible values: 0..8 Range: 0..48 dB Attenuation with 6 dB steps
        return osmosdr::gain_range_t(-48.0,0,6);
    } else if (name == "LNA") {
        // 0 or 1: 1 to activate LNA (alias PreAmp): 1 = +6 dB gain - compensated in digital
        return osmosdr::gain_range_t(0,6,6);
    } else {
        AIRSPYHF_WARNING("airspyhf_source_c failed")
    }
    
    return osmosdr::gain_range_t();
}

/* Gain */

// 0 or 1: 1 to activate LNA (alias PreAmp): 1 = +6 dB gain - compensated in digital
uint8_t _airspyhf_lna_db_to_flag(double db) {
    return db >= 3.0 ? 1 : 0;
}

double _airspyhf_lna_flag_to_db(uint8_t flag) {
    return flag ? 6.0 : 0.0;
}

// Possible values: 0..8 Range: 0..48 dB Attenuation with 6 dB steps
uint8_t _airspyhf_att_db_to_value(double db) {
    return std::round(-db/6.0);
}

double _airspyhf_att_value_to_db(uint8_t value) {
    return value * -6.0;
}

double airspyhf_source_c::set_gain(double gain, size_t chan)
{
    assert(chan == 0);
    return set_gain(gain, "ATT", chan);
}

double airspyhf_source_c::set_gain(double gain, const std::string & name, size_t chan)
{
    int ret;
    assert(chan == 0);
    
    if (name == "ATT") {
        uint8_t att = _airspyhf_att_db_to_value(gain);
        if(_att != att) {
            ret = airspyhf_set_hf_att(_dev, att);
            assert(ret == AIRSPYHF_SUCCESS);
            _att = att;
            AIRSPYHF_INFO("att: " << std::to_string(_att));
        }
        return _airspyhf_att_value_to_db(_att);
    }
    else if (name == "LNA") {
        uint8_t lna = _airspyhf_lna_db_to_flag(gain);
        if(_lna != lna) {
            ret = airspyhf_set_hf_lna(_dev, lna);
            assert(ret == AIRSPYHF_SUCCESS);
            _lna = lna;
            AIRSPYHF_INFO("lna: " << std::to_string(_lna));
        }
        return _airspyhf_lna_flag_to_db(_lna);
    } else {
        AIRSPYHF_WARNING("unknown gain: " << name);
        return 0.0;
    }
}

double airspyhf_source_c::get_gain(size_t chan)
{
    assert(chan == 0);
    return get_gain("ATT", chan);
    
}

double airspyhf_source_c::get_gain(const std::string &name, size_t chan)
{
    assert(chan == 0);
    
    if (name == "ATT") {
        return _airspyhf_att_value_to_db(_att);
    } else if (name == "LNA") {
        return _airspyhf_lna_flag_to_db(_lna);
    } else {
        AIRSPYHF_WARNING("unknown gain: " << name);
        return 0.0;
    }
}

bool airspyhf_source_c::set_gain_mode(bool automatic, size_t chan) {
    int ret;
    
    assert(chan == 0);
    assert(_dev != nullptr);

    if(_agc != automatic) {
        _agc = automatic;
        ret = airspyhf_set_hf_agc(_dev, automatic);
        assert(ret == AIRSPYHF_SUCCESS);
        AIRSPYHF_INFO("AGC: " << std::to_string(_agc));
    }

    return _agc;
}

bool airspyhf_source_c::get_gain_mode(size_t chan) {
    assert(chan == 0);
    return _agc;
}

void airspyhf_source_c::set_iq_balance(const std::complex<double> &balance, size_t chan) {
    int ret;
    float w = std::arg(balance);
    ret = airspyhf_set_optimal_iq_correction_point(_dev, w);
    AIRSPYHF_INFO(std::to_string(w));
    assert(ret == AIRSPYHF_SUCCESS);
}

void airspyhf_source_c::set_clock_source(const std::string &source,
                                         const size_t mboard) {
    // not configurable
}

std::string airspyhf_source_c::get_clock_source(const size_t mboard) {
    return "internal";
}

std::vector<std::string> airspyhf_source_c::get_clock_sources(const size_t mboard) {
    std::vector<std::string> sources;
    sources.push_back(get_clock_source(0));
    return sources;
}

double airspyhf_source_c::get_clock_rate(size_t mboard) {
    return 36.864e6;
}

void airspyhf_source_c::set_clock_rate(double rate, size_t mboard) {
    // not configurable
}

std::vector< std::string > airspyhf_source_c::get_antennas(size_t chan)
{
    assert(chan == 0);
    std::vector<std::string> antennas;
    antennas.push_back(get_antenna(chan));
    return antennas;
}

std::string airspyhf_source_c::set_antenna(const std::string & antenna, size_t chan)
{
    assert(chan == 0);
    return get_antenna(chan);
}

std::string airspyhf_source_c::get_antenna(size_t chan)
{
    return "RX";
}
