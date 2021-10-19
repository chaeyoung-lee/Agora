/**
 * @file radio_lib.h
 * @brief Declaration file for the RadioConfig class.
 */
#ifndef RADIO_LIB_H_
#define RADIO_LIB_H_

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Time.hpp>
#include <chrono>
#include <complex>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "config.h"

class RadioConfig {
 public:
  explicit RadioConfig(Config* cfg);
  bool RadioStart();
  void RadioStop();
  void ReadSensors();
  void RadioTx(void** buffs);
  void RadioRx(void** buffs);
  int RadioTx(size_t /*r*/, void** buffs, int flags, long long& frameTime);
  int RadioRx(size_t /*r*/, void** buffs, long long& frameTime);
  void Go();
  arma::cx_float* GetCalibUl() { return init_calib_ul_processed_; }
  arma::cx_float* GetCalibDl() { return init_calib_dl_processed_; }
  ~RadioConfig();

  // Thread functions
  void InitBsRadio(size_t tid);
  void ConfigureBsRadio(size_t tid);

 private:
  bool InitialCalib(bool sample_adjust, Table<arma::cx_float>& calib_ul,
                    Table<arma::cx_float>& calib_dl);
  static void DrainRxBuffer(SoapySDR::Device* ibsSdrs,
                            SoapySDR::Stream* istream, std::vector<void*> buffs,
                            size_t symSamp);
  void DrainBuffers();
  void AdjustDelays(std::vector<int> /*offset*/);
  static void DciqMinimize(SoapySDR::Device* /*targetDev*/,
                           SoapySDR::Device* /*refDev*/, int /*direction*/,
                           size_t /*channel*/, double /*rxCenterTone*/,
                           double /*txCenterTone*/);
  static void SetIqBalance(SoapySDR::Device* /*dev*/, int /*direction*/,
                           size_t /*channel*/, int /*gcorr*/, int /*iqcorr*/);
  static void AdjustCalibrationGains(std::vector<SoapySDR::Device*> /*rxDevs*/,
                                     SoapySDR::Device* /*txDev*/,
                                     size_t /*channel*/, double /*fftBin*/,
                                     bool plot = false);
  static std::vector<std::complex<float>> SnoopSamples(
      SoapySDR::Device* /*dev*/, size_t /*channel*/, size_t /*readSize*/);
  void DciqCalibrationProc(size_t /*channel*/);

  //Variables
  Config* const cfg_;
  const size_t radio_num_;
  const size_t antenna_num_;

  std::vector<SoapySDR::Device*> hubs_;
  std::vector<SoapySDR::Device*> ba_stn_;
  std::vector<SoapySDR::Stream*> tx_streams_;
  std::vector<SoapySDR::Stream*> rx_streams_;

  arma::cx_float* init_calib_ul_processed_;
  arma::cx_float* init_calib_dl_processed_;
  size_t calib_meas_num_;

  std::atomic<size_t> num_radios_initialized_;
  std::atomic<size_t> num_radios_configured_;
};
#endif  // RADIO_LIB_H_
