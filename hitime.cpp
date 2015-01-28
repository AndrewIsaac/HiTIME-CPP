#include <iostream>
#include <unistd.h>
#include <math.h>
#include <iomanip>
#include <cstdlib>
#include <string>
#include <cmath>
#include <algorithm>

#include "pwiz_tools/common/FullReaderList.hpp"
#include "pwiz/data/msdata/MSDataFile.hpp"
#include "pwiz/analysis/spectrum_processing/SpectrumList_MZWindow.hpp"
//#include <armadillo>
//#include "Numpy.hpp"


/*-----------------------------------------------------------------------*/
/******************************* CONSTANTS *******************************/
/*-----------------------------------------------------------------------*/


// default difference in mass of isotopes
const float default_mz_delta        = 6.0201;
// default m/z tolerance in parts per million
const float default_ppm             = 4.0;
// Full Width Half Maximum in PPM
const float default_fwhm            = 150.0;
const float default_mz_sigma        = 1.5;
// default ratio of isotopes
const float default_intensity_ratio = 1.0;
// default retention time FWHM in scans 
const float default_rt_width        = 17.0;
const float default_rt_sigma        = 1.5;
// minimum number of samples in score regions
const float default_min_sample      = default_rt_width * default_rt_sigma 
                                        / 2.355;
// pi
constexpr double pi() { return std::atan(1) * 4; } 
// sqrt 2pi
const double root2pi = sqrt(2.0 * pi());

/*-----------------------------------------------------------------------*/ 
/************************* FUNCTION DECLARATIONS *************************/
/*-----------------------------------------------------------------------*/

//template <typename T>
//using vect_function = std::vector<T> (*)(std::vector<T>);
//vect_function v;





void show_usage(char *cmd);

//template <class T>
std::vector<double> centre_vector(std::vector<double> vect);

template <typename T, typename F>
std::vector<T> apply_vect_func(std::vector<T> vect, F func);

  
/*-----------------------------------------------------------------------*/
/******************************** CLASSES ********************************/
/*-----------------------------------------------------------------------*/


class Options {

    public:
        float intensity_ratio;
        float rt_width;
        float rt_sigma;
        float ppm;
        float mz_width;
        float mz_sigma;
        float mz_delta;
        float min_sample;
        std::string mzML_file;

        Options(int argc, char *argv[]);        
};


/*-----------------------------------------------------------------------*/
/********************************* MAIN **********************************/
/*-----------------------------------------------------------------------*/


int main(int argc, char *argv[])
{
/*
    Options opts(argc, argv);
   
    const bool getBinaryData = true;
    pwiz::msdata::FullReaderList readers;
    pwiz::msdata::MSDataFile msd(opts.mzML_file, &readers);
    pwiz::msdata::SpectrumList& spectrumList = *msd.run.spectrumListPtr;
    pwiz::msdata::SpectrumPtr spectrum;
    std::vector<pwiz::msdata::MZIntensityPair> mz_mu_pairs;
    pwiz::msdata::MZIntensityPair pair;
    
    float  rt_sigma     = opts.rt_width / 2.355;
    double mz_ppm_sigma = opts.mz_width / 2.355e6;
    int    rt_len       = spectrumList.size();
    int    mid_win      = rt_len / 2;
    pwiz::msdata::SpectrumPtr mz_mu_vect = spectrumList.spectrum(mid_win, 
                                                            getBinaryData);
    double lo_tol = 1.0 - opts.mz_sigma * mz_ppm_sigma;
    double hi_tol = 1.0 + opts.mz_sigma * mz_ppm_sigma;

    std::vector<double> points_lo_lo;
    std::vector<double> points_lo_hi;
    std::vector<double> points_hi_lo;
    std::vector<double> points_hi_hi;

    std::vector<std::vector<double>> data_lo;
    std::vector<std::vector<double>> data_hi;
    std::vector<std::vector<double>> shape_lo;
    std::vector<std::vector<double>> shape_hi;
    std::vector<int> len_lo;
    std::vector<int> len_hi;

    mz_mu_vect->getMZIntensityPairs(mz_mu_pairs);
    for (auto pair : mz_mu_pairs) {
        points_lo_lo.push_back(pair.mz * lo_tol);
        points_lo_hi.push_back(pair.mz * hi_tol);
        points_hi_lo.push_back((pair.mz + opts.mz_delta) * lo_tol);
        points_hi_hi.push_back((pair.mz + opts.mz_delta) * hi_tol);

        std::vector<double> data;
        data_lo.push_back(data);
        data_hi.push_back(data);
        shape_lo.push_back(data);
        shape_hi.push_back(data);
        len_lo.push_back(0);
        len_hi.push_back(0);
    }

    std::vector<float> rt_shape;

    for (int i = 0; i < rt_len; ++i) {

        float pt = (i - mid_win) / rt_sigma;
        pt = -0.5 * pt * pt;
        pt = exp(pt) / (rt_sigma * root2pi); 
    
        rt_shape.push_back(pt);
    }
    
    for (size_t mzi = 0; mzi < mz_mu_pairs.size(); ++mzi) {
    
        double lo_tol_lo = points_lo_lo[mzi];
        double lo_tol_hi = points_lo_hi[mzi];
        double hi_tol_lo = points_hi_lo[mzi];
        double hi_tol_hi = points_hi_hi[mzi];
        double centre    = mz_mu_pairs[mzi].mz;
        double sigma     = centre * mz_ppm_sigma;

        pwiz::analysis::SpectrumList_MZWindow lo_window(
                                                    msd.run.spectrumListPtr,
                                                    lo_tol_lo, lo_tol_hi);
        pwiz::analysis::SpectrumList_MZWindow hi_window(
                                                    msd.run.spectrumListPtr,
                                                    hi_tol_lo, hi_tol_hi);
            
    
        for (int rowi = 0; rowi < rt_len; ++rowi) {
        
            pwiz::msdata::SpectrumPtr lo_spectrum;
            pwiz::msdata::SpectrumPtr hi_spectrum;
            std::vector<pwiz::msdata::MZIntensityPair> lo_pairs;
            std::vector<pwiz::msdata::MZIntensityPair> hi_pairs;
            
            lo_spectrum = lo_window.spectrum(rowi, getBinaryData);
            hi_spectrum = hi_window.spectrum(rowi, getBinaryData);
        
            lo_spectrum->getMZIntensityPairs(lo_pairs);
            hi_spectrum->getMZIntensityPairs(hi_pairs);
        
            float rt_lo = rt_shape[rowi];
            float rt_hi = rt_lo;

            if (lo_pairs.size() > 0) {
            
                for (auto pair : lo_pairs) {
                    float mz = (pair.mz - centre) / sigma;
                    mz = -0.5 * mz * mz;
                    mz = exp(mz) / (sigma * root2pi);
                    shape_lo[mzi].push_back(mz);
                    data_lo[mzi].push_back(pair.intensity);
                }
                len_lo[mzi] += lo_pairs.size();

            } else {
                data_lo[mzi].push_back(0);
                shape_lo[mzi].push_back(rt_lo / (sigma * root2pi));
            }
            
            centre += opts.mz_delta;
            sigma = centre * mz_ppm_sigma;
            
            if (hi_pairs.size() > 0) {
            
                for (auto pair : hi_pairs) {
                    float mz = (pair.mz - centre) / sigma;
                    mz = -0.5 * mz * mz;
                    mz = exp(mz) / (sigma * root2pi);
                    shape_hi[mzi].push_back(mz);
                    data_hi[mzi].push_back(pair.intensity);
                }
                len_hi[mzi] += hi_pairs.size();

            } else {
                data_hi[mzi].push_back(0);
                shape_hi[mzi].push_back(rt_hi / (sigma * root2pi));
            }
        }
    }

    for (size_t leni = 0; leni < len_lo.size(); ++leni) {
        if (len_lo[leni] < opts.min_sample) {
            data_lo[leni]  = {0.0};
            shape_lo[leni] = {0.0};
        }
    }

    for (size_t leni = 0; leni < len_hi.size(); ++leni) {
        if (len_hi[leni] < opts.min_sample) {
            data_lo[leni]  = {0.0};
            shape_lo[leni] = {0.0};
        }
    }

    std::vector<std::vector<double>> dataAB;
    std::vector<std::vector<double>> nAB;

    for (size_t i = 0; i < data_lo.size(); ++i) {
        
        std::vector<double> dataAB_row;
        size_t length_lo = data_lo[i].size();
        size_t length_hi = data_hi[i].size();
        
        for (auto lo_value : data_lo[i]){
            dataAB_row.push_back(lo_value * length_hi);
        }
        for (auto hi_value : data_hi[i]){
            dataAB_row.push_back(hi_value * length_lo);
        }
        dataAB.push_back(dataAB_row);

        std::vector<double> nAB_row;
        nAB_row.push_back(length_lo + length_hi);
        nAB.push_back(nAB_row);
    }

    std::vector<std::vector<double>> shapeAB;
    std::vector<std::vector<double>> shapeA0;
    std::vector<std::vector<double>> shapeB0;
    std::vector<std::vector<double>> shape1r;

    for (size_t i = 0; i < shape_lo.size(); ++i) {
        
        std::vector<double> shapeAB_row;
        std::vector<double> shapeA0_row;
        std::vector<double> shapeB0_row;
        std::vector<double> shape1r_row;
        size_t length_lo = shape_lo[i].size();
        size_t length_hi = shape_hi[i].size();
        
        for (auto lo_value : shape_lo[i]){
            shapeAB_row.push_back(lo_value * length_hi);
            shapeA0_row.push_back(lo_value * length_hi);
            shapeB0_row.push_back(0.0);
            shape1r_row.push_back(length_hi);
        }
        for (auto hi_value : shape_hi[i]){
            shapeAB_row.push_back(hi_value * length_lo);
            shapeA0_row.push_back(0.0);
            shapeB0_row.push_back(hi_value * length_lo);
            shape1r_row.push_back(opts.intensity_ratio * length_lo);
        }
        
        shapeAB.push_back(shapeAB_row);
        shapeA0.push_back(shapeA0_row);
        shapeB0.push_back(shapeB0_row);
        shape1r.push_back(shape1r_row);
    } 
*/
    std::vector<double> x = {1 ,5, 2, 5, 2, 3,2, 5};
    std::vector<std::vector<double>> xx;

    for (int i = 0; i < 5; ++i){
        xx.push_back(x);
    }

    xx = apply_vect_func(xx, centre_vector);
    
    //for (size_t i = 0; i < xx.size(); ++i) {
    //    xx[i] = centre_vector(xx[i]);
    //}
    
    for (auto a : xx){
        std::cout << "NEW VECTOR: " << std::endl;
        for (auto b : a) {
            std::cout << b << std::endl;
        }
    }

    std::cout << "Done!" << std::endl;
    return 0;
}


/*-----------------------------------------------------------------------*/
/************************* FUNCTION DEFINITIONS **************************/
/*-----------------------------------------------------------------------*/


void show_usage(char *cmd)
{
    using namespace std;

    cout << "Usage:     " << cmd << " [-options] [arguments]"       << endl;
    cout                                                            << endl;
    cout << "options:   " << "-h  show this help information"       << endl;
    cout << "           " << "-i  ratio of doublet intensities (isotope \n";
    cout << "           " << "    / parent)"                        << endl;
    cout << "           " << "-r  full width at half maximum for \n"       ;
    cout << "           " << "    retention time in number of scans"<< endl;
    cout << "           " << "-R  retention time width boundary in \n"     ;
    cout << "           " << "    standard deviations"              << endl;
    cout << "           " << "-p  m/z tolerance in parts per million"      ;
    cout                                                            << endl;
    cout << "           " << "-m  m/z full width at half maximum in \n"    ;
    cout << "           " << "    parts per million"                << endl;
    cout << "           " << "-M  m/z window boundary in standard \n"      ;
    cout << "           " << "    deviations"                       << endl;
    cout << "           " << "-D  m/z difference for doublets"      << endl;
    cout << "           " << "-s  minimum number of data points \n"        ;
    cout << "           " << "    required in each sample region"   << endl;
    cout                                                            << endl;
    cout << "arguments: " << "mzML_file     path to mzML file"      << endl;
    cout                                                            << endl;
    cout << "example:   " << cmd << " example.mzML"                 << endl;
    cout                                                            << endl;
}

//template <class T> 
std::vector<double> centre_vector(std::vector<double> vect)
{
    double sum  = std::accumulate(vect.begin(), vect.end(), 0.0);
    double mean = sum / vect.size();
    std::vector<double> centered;

    for (auto v : vect) {
        centered.push_back(v - mean);
    }

    return centered;
}

template <typename T, typename F>
std::vector<T> apply_vect_func(std::vector<T> vect, F func)
{
    std::vector<T> applied;
    
    for (auto v : vect) {
        applied.push_back(func(v));
    }

    return applied;
}

/*-----------------------------------------------------------------------*/
/***************************** CLASS METHODS *****************************/
/*-----------------------------------------------------------------------*/


Options::Options(int argc, char *argv[])
{
    char opt;
    int opt_idx;

    intensity_ratio = default_intensity_ratio;
    rt_width        = default_rt_width;
    rt_sigma        = default_rt_sigma;
    ppm             = default_ppm;
    mz_width        = default_fwhm;
    mz_sigma        = default_mz_sigma;
    mz_delta        = default_mz_delta;
    min_sample      = default_min_sample;
    mzML_file       = "";

    // Show usage and exit if no options are given
    if (argc == 1) {
        show_usage(argv[0]);
        exit(1);
    }

    while ((opt = getopt(argc, argv, "hd:i:r:R:p:m:M:D:s:")) != -1){
        
        switch (opt) {
            case 'h':
                show_usage(argv[0]);
                exit(1);
                break;
            case 'i':
                intensity_ratio = std::stof(std::string(optarg));
                break;
            case 'r':
                rt_width = std::stof(std::string(optarg));
                break;
            case 'R':
                rt_sigma = std::stof(std::string(optarg));
                break;
            case 'p':
                ppm = std::stof(std::string(optarg));
                break;
            case 'm':
                mz_width = std::stof(std::string(optarg));
                break;
            case 'M':
                mz_sigma = std::stof(std::string(optarg));
                break;
            case 'D':
                mz_delta = std::stof(std::string(optarg));
                break;
            case 's':
                min_sample = std::stof(std::string(optarg));
                break;
        }
    }

    for (opt_idx = optind; opt_idx < argc; opt_idx++) {

        if (mzML_file == "") { 
            mzML_file = argv[opt_idx];
        } else {
            std::cout << "Too many arguments supplied. See usage.";
            std::cout << std::endl;
            exit(1);
        }
    }

    if (mzML_file == "") {
        std::cout << "Insufficient arguments supplies. See usage.";
        std::cout << std::endl;
        exit(1);
    }
}


/*-----------------------------------------------------------------------*/
/******************************* OLD CODE ********************************/
/*-----------------------------------------------------------------------*/

/*
    Options opts(argc, argv);
   
    pwiz::msdata::FullReaderList readers;
    pwiz::msdata::MSDataFile msd(opts.mzML_file, &readers);

    
    std::cout << "Timestamp: " << msd.run.startTimeStamp << std::endl;

    pwiz::msdata::SpectrumList& spectrumList = *msd.run.spectrumListPtr;
    const bool getBinaryData = true;
    size_t numSpectra = spectrumList.size();

    std::cout << "Num Spectra: " << numSpectra << std::endl;
    
    pwiz::msdata::SpectrumPtr spectrum;
    std::vector<pwiz::msdata::MZIntensityPair> pairs;
    spectrum = spectrumList.spectrum(0, getBinaryData);
    spectrum->getMZIntensityPairs(pairs);

    std::cout << "Num Pairs: " << pairs.size() << std::endl;

    pwiz::msdata::MZIntensityPair pair;
    pair = pairs[0];

    std::cout << "MZ/Int Pair: " << pair.mz << " " << pair.intensity; 
    std::cout << std::endl;

    pwiz::analysis::SpectrumList_MZWindow mz_window(msd.run.spectrumListPtr,
                                                    150.2, 150.3);
    spectrum = mz_window.spectrum(0, getBinaryData);
    spectrum->getMZIntensityPairs(pairs);
    pair = pairs[0];

    std::cout << "WINDOW 1 (150.2 - 150.3): " << std::endl;
    std::cout << "Num Pairs: " << pairs.size() << std::endl;
    std::cout << "MZ/Int Pair: " << pair.mz << " " << pair.intensity; 
    std::cout << std::endl;

    std::cout << "Done!" << std::endl;
*/

