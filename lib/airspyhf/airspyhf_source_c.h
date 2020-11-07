/* -*- c++ -*- */
/*
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * This file is part of GNU Radio
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
#ifndef INCLUDED_AIRSPYHF_SOURCE_C_H
#define INCLUDED_AIRSPYHF_SOURCE_C_H

#include <mutex>
#include <condition_variable>
#include <gnuradio/sync_block.h>
#include <libairspyhf/airspyhf.h>
#include "source_iface.h"

class airspyhf_source_c;

typedef boost::shared_ptr<airspyhf_source_c> airspyhf_source_c_sptr;

/*!
 * \brief Return a shared_ptr to a new instance of airspyhf_source_c.
 *
 * To avoid accidental use of raw pointers, airspyhf_source_c's
 * constructor is private.  make_airspyhf_source_c is the public
 * interface for creating new instances.
 */
airspyhf_source_c_sptr make_airspyhf_source_c (const std::string & args = "");

/*!
 * \brief Provides a stream of complex samples.
 * \ingroup block
 */
class airspyhf_source_c: public gr::sync_block, public source_iface
{
private:
    // The friend declaration allows make_airspyhf_source_c to
    // access the private constructor.
    friend airspyhf_source_c_sptr make_airspyhf_source_c (const std::string & args);
    airspyhf_source_c (const std::string & args);
    
public:
    ~airspyhf_source_c ();
    
    bool start();
    bool stop();
    
    int work( int noutput_items,
             gr_vector_const_void_star &input_items,
             gr_vector_void_star &output_items );
    
    static std::vector< std::string > get_devices();
    
    size_t get_num_channels(void);
    
    osmosdr::meta_range_t get_sample_rates(void);
    double set_sample_rate(double rate);
    double get_sample_rate(void);
    
    osmosdr::freq_range_t get_freq_range(size_t chan = 0);
    double set_center_freq(double freq, size_t chan = 0);
    double get_center_freq(size_t chan = 0);
    double set_freq_corr(double ppm, size_t chan = 0);
    double get_freq_corr(size_t chan = 0);
    
    std::vector<std::string> get_gain_names(size_t chan = 0);
    osmosdr::gain_range_t get_gain_range(size_t chan = 0);
    osmosdr::gain_range_t get_gain_range(const std::string & name, size_t chan = 0);
    double set_gain(double gain, size_t chan = 0);
    double set_gain(double gain, const std::string & name, size_t chan = 0);
    double get_gain(size_t chan = 0);
    double get_gain(const std::string & name, size_t chan = 0);
    
    bool set_gain_mode(bool automatic, size_t chan = 0);
    bool get_gain_mode(size_t chan = 0);
    
    void set_iq_balance(const std::complex<double> &balance, size_t chan = 0);
    
    std::vector<std::string> get_antennas( size_t chan = 0);
    std::string set_antenna(const std::string & antenna, size_t chan = 0);
    std::string get_antenna(size_t chan = 0);
    
private:
    airspyhf_device *_dev;
    uint32_t _airspyhf_output_size;

    bool   _agc_on;
    double _att_gain;
    double _lna_gain;
    
    static int _airspyhf_rx_callback(airspyhf_transfer_t* transfer);
    int airspyhf_rx_callback(airspyhf_transfer_t *transfer);
        
    //boost::circular_buffer<gr_complex> *_fifo;
    //std::mutex _fifo_lock;
    //std::condition_variable _samp_avail;
    
    //std::vector< std::pair<double, uint32_t> > _sample_rates;
    std::vector<uint32_t> _samplerates;
    double _sample_rate;
    double _center_freq;
    double _freq_corr;
    
    uint64_t _dropped_samples;
    void *_stream_buff;
    std::mutex _stream_mutex;
    std::condition_variable _stream_cond;
    std::condition_variable _callback_done_cond;
};

#endif /* INCLUDED_AIRSPY_SOURCE_C_H */
