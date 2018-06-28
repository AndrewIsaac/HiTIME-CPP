#include <OpenMS/FORMAT/IndexedMzMLFileLoader.h>
#include <OpenMS/KERNEL/Peak1D.h>
#include <mutex>
#include <iostream>
#include <map>
#include <thread>
#include "vector.h"
#include "options.h"
#include "constants.h"
#include "lru_cache.hpp"
#include "score.h"

// input cache size
#define CACHE_SIZE 30

using namespace OpenMS;
using namespace std;

mutex output_spectrum_lock;
mutex next_spectrum_lock;
mutex input_spectrum_lock;


Scorer::Scorer(bool debug, double intensity_ratio, double rt_width, double rt_sigma, double ppm,
               double mz_width, double mz_sigma, double mz_delta, double min_sample, int num_threads,
               string in_file, string out_file)
   : debug(debug)
   , intensity_ratio(intensity_ratio)
   , rt_width(rt_width)
   , rt_sigma(rt_sigma)
   , ppm(ppm)
   , mz_width(mz_width)
   , mz_sigma(mz_sigma)
   , mz_delta(mz_delta)
   , min_sample(min_sample)
   , num_threads(num_threads)
   , in_file(in_file)
   , out_file(out_file)
   , input_spectrum_cache(CACHE_SIZE)
   , spectrum_writer(out_file)
   , current_spectrum_id{0}
   , next_output_spectrum_id{0}
{

   IndexedMzMLFileLoader mzml;

   mzml.load(in_file, input_map);

   half_window = ceil(rt_sigma * rt_width / std_dev_in_fwhm);
   num_spectra = input_map.getNrSpectra();

   vector<thread> threads(num_threads);

   cout << "Num threads: " << num_threads << endl;
   cout << "Num spectra: " << num_spectra << endl;

   for (int thread_count = 0; thread_count < num_threads; thread_count++)
   {
      threads[thread_count] = thread(&Scorer::score_worker, this, thread_count);
    
   }

   for (int thread_count = 0; thread_count < num_threads; thread_count++)
   {
       threads[thread_count].join();
   }
}

/*
PeakSpectrumPtr Scorer::get_spectrum(int spectrum_id)
{
   PeakSpectrumPtr spectrum_ptr;

   input_spectrum_lock.lock();
   spectrum_ptr = make_shared<PeakSpectrum>(input_map.getSpectrum(spectrum_id));
   input_spectrum_lock.unlock();
   return spectrum_ptr;
}
*/
PeakSpectrumPtr Scorer::get_spectrum(int spectrum_id)
{
   PeakSpectrumPtr spectrum_ptr;

   input_spectrum_lock.lock();
   if (input_spectrum_cache.exists(spectrum_id))
   {
      spectrum_ptr = input_spectrum_cache.get(spectrum_id);
   }
   else
   {
      spectrum_ptr = make_shared<PeakSpectrum>(input_map.getSpectrum(spectrum_id));
      input_spectrum_cache.put(spectrum_id, spectrum_ptr);
   }
   input_spectrum_lock.unlock();
   return spectrum_ptr;
}

void Scorer::put_spectrum(int spectrum_id, PeakSpectrum spectrum)
{
   output_spectrum_lock.lock();

   if (spectrum_id == next_output_spectrum_id)
   {
      // this is the next spectrum to output
      //cout << spectrum_id << endl;
      spectrum_writer.consumeSpectrum(spectrum);
      next_output_spectrum_id++;

      // try to output more spectra
      while(output_spectrum_queue.size() > 0)
      {
         IndexSpectrum index_spectrum = output_spectrum_queue.top();
         if (index_spectrum.first == next_output_spectrum_id)
         {
            spectrum_writer.consumeSpectrum(index_spectrum.second);
            //cout << index_spectrum.first << endl;
            output_spectrum_queue.pop();
            next_output_spectrum_id++;
         }
         else
         {
            break;
         }
      }
   }
   else
   {
      // push this spectrum into the queue to write out later
      output_spectrum_queue.push(pair<int, PeakSpectrum>(spectrum_id, spectrum));
   } 

   output_spectrum_lock.unlock();
}

int Scorer::get_next_spectrum_todo(void)
{
   int this_spectrum;
   next_spectrum_lock.lock();
   this_spectrum = current_spectrum_id;
   current_spectrum_id++;
   next_spectrum_lock.unlock();
   return this_spectrum; 
}

void Scorer::score_worker(int thread_count)
{
   double_vect score;
   int this_spectrum_id;

   this_spectrum_id = get_next_spectrum_todo(); 

   while (this_spectrum_id < num_spectra)
   {
//       cout << "Thread: " << thread_count << " Spectrum: " << this_spectrum_id << endl;

       score = score_spectra(this_spectrum_id);
       PeakSpectrumPtr input_spectrum = get_spectrum(this_spectrum_id);
       
       PeakSpectrum output_spectrum = MSSpectrum<Peak1D>(*input_spectrum);
       for (int index = 0; index < input_spectrum->size(); index++)
       {
           output_spectrum[index].setIntensity(score[index]);
           //output_spectrum[index].setIntensity(42);
       }

       put_spectrum(this_spectrum_id, output_spectrum);
       
       this_spectrum_id = get_next_spectrum_todo(); 
   }

}


/*! Calculate correlation scores for each MZ point in a central spectrum of
 * a data window.
 *
 * @param map collection of spectra in the input.
 * @param centre_idx Index of the spectrum to score.
 * @param half_window The number of spectra each side of the central spectrum
 * to include.
 * @param opts Options object.
 *
 * @return Vector of score at each MZ in central spectrum.
 */

double_vect Scorer::score_spectra(int centre_idx)
{
    // Calculate constant values
    double mz_delta_opt = mz_delta;
    double min_sample_opt = min_sample;
    double intensity_ratio_opt = intensity_ratio;
    double rt_sigma = rt_width / std_dev_in_fwhm;
    double mz_ppm_sigma = mz_width / (std_dev_in_fwhm * 1e6);
    Size rt_len = input_map.getNrSpectra();
    double lower_tol = 1.0 - mz_sigma * mz_ppm_sigma;
    double upper_tol = 1.0 + mz_sigma * mz_ppm_sigma;
    int rt_offset = centre_idx - half_window;

    // XXX This should be calculated once, then used as look-up
    // Spacing should also be based on scan intervals
    // (curently assumed fixed spacing)
    std::vector<double> rt_shape;

    // Calculate Gaussian shape in the RT direction
    for (int i = 0; i < (2 * half_window) + 1; ++i)
    {
        double pt = (i - half_window) / rt_sigma;
        pt = -0.5 * pt * pt;
        double fit = exp(pt) / (rt_sigma * root2pi);
        rt_shape.push_back(fit);
    }

    PeakSpectrumPtr centre_row_points = get_spectrum(centre_idx);

    // Length of all vectors (= # windows)
    size_t mz_windows = centre_row_points->size();

    // Low (natural ion) peak tolerances
    double lower_bound_nat = 0.0;
    double upper_bound_nat = 0.0;
//    double_vect lower_bound_nat;
//    double_vect upper_bound_nat;
    // High (isotope ion) peak tolerances
    double lower_bound_iso = 0.0;
    double upper_bound_iso = 0.0;
//    double_vect lower_bound_iso;
//    double_vect upper_bound_iso;

    // total data per rt,mz centre
    double nAB = 0.0;
//    double_vect nAB;

    double_vect data_nat;
    double_vect data_iso;
    double_vect shape_nat;
    double_vect shape_iso;
//    double_2d data_nat;
//    double_2d data_iso;
//    double_2d shape_nat;
//    double_2d shape_iso;

/*** Now main loop per single mz ***/
    // Calculate tolerances for the lo and hi peak for each central MZ
    PeakSpectrum::Iterator it;
    for (it = centre_row_points->begin(); it != centre_row_points->end(); ++it)
    {
//        double this_mz = it->getMZ();
        double centre = it->getMZ();
        lower_bound_nat = centre * lower_tol;
        upper_bound_nat = centre * upper_tol;
        lower_bound_iso = (centre + mz_delta_opt) * lower_tol;
        upper_bound_iso = (centre + mz_delta_opt) * upper_tol;
//        lower_bound_nat.push_back(this_mz * lower_tol);
//        upper_bound_nat.push_back(this_mz * upper_tol);
//        lower_bound_iso.push_back((this_mz + mz_delta_opt) * lower_tol);
//        upper_bound_iso.push_back((this_mz + mz_delta_opt) * upper_tol);

//        double_vect data;
//        data_nat.push_back(data);
//        data_iso.push_back(data);
//        shape_nat.push_back(data);
//        shape_iso.push_back(data);
//    }

                double sigma = centre * mz_ppm_sigma;

    // Iterate over the spectra in the window
    for (int rowi = centre_idx - half_window; rowi <= centre_idx + half_window; ++rowi)
    {
        // window can go outside start and end of scans, so check bounds
        if (rowi >= 0 && rowi < rt_len)
        {
            double rt_shape_nat = rt_shape[rowi - rt_offset];
            double rt_shape_iso = rt_shape_nat * intensity_ratio_opt;

            PeakSpectrumPtr rowi_spectrum;
            rowi_spectrum = get_spectrum(rowi);
            // Could handle by sorting, but this shouldn't happen, so want to
            // know if it does
            // XXX maybe we should provide an option to avoid this for performance reasons?
            if (!rowi_spectrum->isSorted()) throw std::runtime_error ("Spectrum not sorted");

            // Iterate over the points in the central spectrum
//            for (size_t mzi = 0; mzi < mz_windows; ++mzi)
//            {
                // Get the tolerances and value for this point
//                double lower_tol_nat = lower_bound_nat[mzi];
//                double upper_tol_nat = upper_bound_nat[mzi];
//                double lower_tol_iso = lower_bound_iso[mzi];
//                double upper_tol_iso = upper_bound_iso[mzi];
//                double centre = (*centre_row_points)[mzi].getMZ();
//                double sigma = centre * mz_ppm_sigma;
    
                // Select points within tolerance for current spectrum
    
                // Want index of bounds
                Size lower_index = Size(rowi_spectrum->MZBegin(lower_bound_nat) - rowi_spectrum->begin());
                Size upper_index = Size(rowi_spectrum->MZBegin(upper_bound_nat) - rowi_spectrum->begin());
//                Size lower_index = Size(rowi_spectrum->MZBegin(lower_tol_nat) - rowi_spectrum->begin());
//                Size upper_index = Size(rowi_spectrum->MZBegin(upper_tol_nat) - rowi_spectrum->begin());
    
                // Check if points found...
                if (lower_index <= upper_index)
                {
                    // Calculate Gaussian value for each found MZ
                    for (Size index = lower_index; index <= upper_index; ++index)
                    {
                        Peak1D peak = (*rowi_spectrum)[index];
                        double mz = peak.getMZ();
                        double intensity = peak.getIntensity();
    
                        // just in case
                        if (mz < lower_bound_nat || mz > upper_bound_nat) continue;
//                        if (mz < lower_tol_nat || mz > upper_tol_nat) continue;
    
                        // calc mz fit
                        mz = (mz - centre) / sigma;
                        mz = -0.5 * mz * mz;
                        double fit = exp(mz) / (sigma * root2pi);
    
                        data_nat.push_back(intensity);
                        shape_nat.push_back(fit * rt_shape_nat);
//                        data_nat[mzi].push_back(intensity);
//                        shape_nat[mzi].push_back(fit * rt_shape_nat);
                    }
                }

                // Increment centre to isotope peak
                centre += mz_delta_opt;
                sigma = centre * mz_ppm_sigma;

                // Select points within tolerance for current spectrum
                lower_index = Size(rowi_spectrum->MZBegin(lower_bound_iso) - rowi_spectrum->begin());
                upper_index = Size(rowi_spectrum->MZBegin(upper_bound_iso) - rowi_spectrum->begin());
//                lower_index = Size(rowi_spectrum->MZBegin(lower_tol_iso) - rowi_spectrum->begin());
//                upper_index = Size(rowi_spectrum->MZBegin(upper_tol_iso) - rowi_spectrum->begin());

                // Check if points found...
                if (lower_index <= upper_index)
                {
                    // Calculate Gaussian value for each found MZ
                    for (Size index = lower_index; index <= upper_index; ++index)
                    {
                        Peak1D peak = (*rowi_spectrum)[index];
                        double mz = peak.getMZ();
                        double intensity = peak.getIntensity();

                        // just in case
                        if (mz < lower_bound_iso || mz > upper_bound_iso) continue;
//                        if (mz < lower_tol_iso || mz > upper_tol_iso) continue;

                        // calc mz fit
                        mz = (mz - centre) / sigma;
                        mz = -0.5 * mz * mz;
                        double fit = exp(mz) / (sigma * root2pi);

                        data_iso.push_back(intensity);
                        shape_iso.push_back(fit * rt_shape_iso);
//                        data_iso[mzi].push_back(intensity);
//                        shape_iso[mzi].push_back(fit * rt_shape_iso);
                    }
                }
//            }  // for (size_t mzi
        }
    }

    // Ignore regions with insufficient number of samples
    // If any region is ignored, set all to empty
        if (data_nat.size() < min_sample_opt ||
                       data_iso.size() < min_sample_opt) {
            data_nat = {};
            shape_nat = {};
            nAB = 0;
        }
        else
        {
            nAB = data_nat.size() + data_iso.size();
        }
//    for (size_t i = 0; i < mz_windows; ++i) {
//        if (data_nat[i].size() < min_sample_opt ||
//                       data_iso[i].size() < min_sample_opt) {
//            data_nat[i] = {};
//            shape_nat[i] = {};
//            nAB.push_back(0);
//        }
//        else
//        {
//            nAB.push_back(data_nat[i].size() + data_iso[i].size());
//        }
//    }

    /*
     * Competing models
     * Low ion window, High ion window
     * Empty low ion window, flat high ion window
     */

    /* Formulation
     * Correlation based on expectations in each region
     * Low ion region, a
     * High ion region, b
     * Correlation is
     * Covariance = E(E((Xa - E(Xab))(Ya - E(Yab))), E((Xb - E(Xab))(Yb -E(Yab))))
     * Data Variance = E(E((Xa - E(Xab))^2), E((Xb - E(Xab))^2))
     * Model Variance = E(E((Ya - E(Yab))^2), E((Yb - E(Yab))^2))
     */

    // Region means
    double EXa;    // E(Xa) 
    double EXb;    // E(Xb)
    double EYa;    // E(Ya)
    double EYb;    // E(Yb)
//    double_vect EXa;    // E(Xa) 
//    double_vect EXb;    // E(Xb)
//    double_vect EYa;    // E(Ya)
//    double_vect EYb;    // E(Yb)

    EXa = mean_vector(data_nat);
    EXb = mean_vector(data_iso);
    EYa = mean_vector(shape_nat);
    EYb = mean_vector(shape_iso);
//    EXa = reduce_2D_vect(data_nat, mean_vector);
//    EXb = reduce_2D_vect(data_iso, mean_vector);
//    EYa = reduce_2D_vect(shape_nat, mean_vector);
//    EYb = reduce_2D_vect(shape_iso, mean_vector);

    // Combined mean is mean of means
    double EXab;    // E(Xab) =  E(E(Xa), E(Xb))
    double EYab;    // E(Yab) =  E(E(Ya), E(Yb))
    EXab = mean_scalars(EXa, EXb);
    EYab = mean_scalars(EYa, EYb);
//    double_vect EXab;    // E(Xab) =  E(E(Xa), E(Xb))
//    double_vect EYab;    // E(Yab) =  E(E(Ya), E(Yb))
//    EXab = apply_vect_func(EXa, EXb, mean_scalars);
//    EYab = apply_vect_func(EYa, EYb, mean_scalars);

    //// FOR TESTING
    // low correlation
    //EXab = EXa;
    //EYab = EYa;
    // high correlation
    //EXab = EXb;
    //EYab = EYb;

    // Centre data in regions relative to combined means
    double_vect CXa;    // (Xa - E(Xab)) --> C(Xa)
    double_vect CXb;    // (Xb - E(Xab))
    double_vect CYa;    // (Ya - E(Yab))
    double_vect CYb;    // (Yb - E(Yab))
//    double_2d CXa;    // (Xa - E(Xab)) --> C(Xa)
//    double_2d CXb;    // (Xb - E(Xab))
//    double_2d CYa;    // (Ya - E(Yab))
//    double_2d CYb;    // (Yb - E(Yab))

    // Centre data in regions relative to combined means
    CXa = shift_vector(data_nat, EXab);
    CXb = shift_vector(data_iso, EXab);
    CYa = shift_vector(shape_nat, EYab);
    CYb = shift_vector(shape_iso, EYab);

    // Square vectors
    double_vect CXa2;    // (Xa - E(Xab))^2 --> C(Xa)^2
    double_vect CYa2;    // (Ya - E(Yab))^2
//    double_2d CXa2;    // (Xa - E(Xab))^2 --> C(Xa)^2
//    double_2d CYa2;    // (Ya - E(Yab))^2

    double_vect CXb2;    // (Xb - E(Xab))^2
    double_vect CYb2;    // (Yb - E(Yab))^2
//    double_2d CXb2;    // (Xb - E(Xab))^2
//    double_2d CYb2;    // (Yb - E(Yab))^2

    CXa2 = square_vector(CXa);
    CXb2 = square_vector(CXb);
    CYa2 = square_vector(CYa);
    CYb2 = square_vector(CYb);

    // Products
    double_vect CXaCYa;    // (Xa - E(Xab))(Ya - E(Yab))
    double_vect CXbCYb;    // (Xb - E(Xab))(Yb - E(Yab))
//    double_2d CXaCYa;    // (Xa - E(Xab))(Ya - E(Yab))
//    double_2d CXbCYb;    // (Xb - E(Xab))(Yb - E(Yab))

    CXaCYa = mult_vectors(CXa, CYa);
    CXbCYb = mult_vectors(CXb, CYb);
//    CXaCYa = apply_vect_func(CXa, CYa, mult_vectors);
//    CXbCYb = apply_vect_func(CXb, CYb, mult_vectors);

    // region expected values
    double ECXaCYa;    // E((Xa - E(Xab))(Ya - E(Yab)))
    double ECXa2;    // E((Xa - E(Xab))^2)
    double ECYa2;    // E((Ya - E(Yab))^2)
//    double_vect ECXaCYa;    // E((Xa - E(Xab))(Ya - E(Yab)))
//    double_vect ECXa2;    // E((Xa - E(Xab))^2)
//    double_vect ECYa2;    // E((Ya - E(Yab))^2)

    double ECXbCYb;    // E((Xb - E(Xab))(Yb - E(Yab)))
    double ECXb2;    // E((Xb - E(Xab))^2)
    double ECYb2;    // E((Yb - E(Yab))^2)
//    double_vect ECXbCYb;    // E((Xb - E(Xab))(Yb - E(Yab)))
//    double_vect ECXb2;    // E((Xb - E(Xab))^2)
//    double_vect ECYb2;    // E((Yb - E(Yab))^2)

    ECXaCYa = mean_vector(CXaCYa);
    ECXbCYb = mean_vector(CXbCYb);
    ECXa2 = mean_vector(CXa2);
    ECXb2 = mean_vector(CXb2);
    ECYa2 = mean_vector(CYa2);
    ECYb2 = mean_vector(CYb2);

    // Variance, Covariance
    double cov_Xab;
    double var_Xab;
    double var_Yab;
//    double_vect cov_Xab;
//    double_vect var_Xab;
//    double_vect var_Yab;

    cov_Xab = mean_scalars(ECXaCYa, ECXbCYb);
    var_Xab = mean_scalars(ECXa2, ECXb2);
    var_Yab = mean_scalars(ECYa2, ECYb2);

    /* Alternate models */
    // low region and high region modelled as flat
    // Region means
    // For high region modelled as all zero, Y_
    // E(Yb) --> E(Y_) = 0 if model region b as all zero

    // Combined mean is mean of means
    double EYa_;    // E(Ya_) =  E(E(Ya), E(Y_)) = 1/2 E(Ya)
    EYa_ = 0.5 * EYa;
//    double_vect EYa_;    // E(Ya_) =  E(E(Ya), E(Y_)) = 1/2 E(Ya)
//    for (const auto& val : EYa) EYa_.push_back(0.5 * val);

    // Centre data in regions relative to combined means
    double_vect CYa_;    // (Ya - E(Ya_))
//    double_2d CYa_;    // (Ya - E(Ya_))
    // (Yb - E(Ya_)) = -E(Ya_)

    // Centre data in regions relative to combined means
    CYa_ = shift_vector(shape_nat, EYa_);
//    CYa_ = apply_vect_func(shape_nat, EYa_, shift_vector);

    // Square vectors
    double_vect CYa_2;    // (Ya - E(Ya_))^2
//    double_2d CYa_2;    // (Ya - E(Ya_))^2
    // (Yb - E(Ya_))^2 = E(Ya_)^2

    CYa_2 = square_vector(CYa_);

    // Products
    double_vect CXaCYa_;    // (Xa - E(Xab))(Ya - E(Ya_))
//    double_2d CXaCYa_;    // (Xa - E(Xab))(Ya - E(Ya_))
    // (Xb - E(Xab))(Yb - E(Ya_)) --> -E(Ya_)*(Xb - E(Xab))

    CXaCYa_ = mult_vectors(CXa, CYa_);

    // region expected values
    double ECXaCYaEa_;    // E((Xa - E(Xab))(Ya - E(Ya_)))
    double ECYaEa_2;    // E((Ya - E(Ya_))^2)
//    double_vect ECXaCYaEa_;    // E((Xa - E(Xab))(Ya - E(Ya_)))
//    double_vect ECYaEa_2;    // E((Ya - E(Ya_))^2)

    double ECXbCYbEa_;    // E((Xb - E(Xab))(Yb - E(Ya_))) = -E(Ya_)*E(Xb - E(Xab))
    double ECYbEa_2;    // E((Yb - E(Ya_))^2) = E(Ya_)^2
//    double_vect ECXbCYbEa_;    // E((Xb - E(Xab))(Yb - E(Ya_))) = -E(Ya_)*E(Xb - E(Xab))
//    double_vect ECYbEa_2;    // E((Yb - E(Ya_))^2) = E(Ya_)^2

    ECXaCYaEa_ = mean_vector(CXaCYa_);
    
    ECXbCYbEa_ = mean_vector(CXb);    // E(Xb - E(Xab))
    ECXbCYbEa_ = mult_scalars(ECXbCYbEa_, EYa_);  // E(Ya_)*E(Xb - E(Xab))
    ECXbCYbEa_ = -ECXbCYbEa_;

    ECYaEa_2 = mean_vector(CYa_2);
    ECYbEa_2 = mult_scalars(EYa_, EYa_);  // E(Ya_)^2
//    ECYaEa_2 = reduce_2D_vect(CYa_2, mean_vector);
//    ECYbEa_2 = apply_vect_func(EYa_, EYa_, mult_scalars);  // E(Ya_)^2

    // Variance, Covariance
    double cov_Xa_;
    double var_Ya_;
//    double_vect cov_Xa_;
//    double_vect var_Ya_;

    cov_Xa_ = mean_scalars(ECXaCYaEa_, ECXbCYbEa_);
    var_Ya_ = mean_scalars(ECYaEa_2, ECYbEa_2);

    // For low region modelled as all zero, Y_
    // E(Ya) --> E(Y_) = 0 if model region a as all zero

    // Combined mean is mean of means
    double EY_b;    // E(Y_b) =  E(E(Y_), E(Yb)) = 1/2 E(Yb)
//    double_vect EY_b;    // E(Y_b) =  E(E(Y_), E(Yb)) = 1/2 E(Yb)
    EY_b = 0.5 * EYb;
//    for (const auto& val : EYb) EY_b.push_back(0.5 * val);

    // Centre data in regions relative to combined means
    double_vect CY_b;    // (Yb - E(Y_b))
//    double_2d CY_b;    // (Yb - E(Y_b))
    // (Ya - E(Y_b)) = -E(Y_b)

    // Centre data in regions relative to combined means
    CY_b = shift_vector(shape_iso, EY_b);
//    CY_b = apply_vect_func(shape_iso, EY_b, shift_vector);

    // Square vectors
    double_vect CY_b2;    // (Yb - E(Y_b))^2
//    double_2d CY_b2;    // (Yb - E(Y_b))^2
    // (Ya - E(Y_b))^2 = E(Y_b)^2

    CY_b2 = square_vector(CY_b);
//    CY_b2 = apply_vect_func(CY_b, square_vector);

    // Products
    // (Xa - E(Xab))(Ya - E(Y_a)) --> -E(Y_a)*(Xa - E(Xab))
    double_vect CXbCY_b;    // (Xb - E(Xab))(Yb - E(Y_b))
//    double_2d CXbCY_b;    // (Xb - E(Xab))(Yb - E(Y_b))

    CXbCY_b = mult_vectors(CXb, CY_b);
//    CXbCY_b = apply_vect_func(CXb, CY_b, mult_vectors);

    // region expected values
    double ECXbCYbE_b;    // E((Xb - E(Xab))(Yb - E(Y_b)))
    double ECYbE_b2;    // E((Yb - E(Y_b))^2)
//    double_vect ECXbCYbE_b;    // E((Xb - E(Xab))(Yb - E(Y_b)))
//    double_vect ECYbE_b2;    // E((Yb - E(Y_b))^2)

    double ECXaCYaE_b;    // E((Xa - E(Xab))(Ya - E(Y_b))) = -E(Y_b)*E(Xa - E(Xab))
    double ECYaE_b2;    // E((Ya - E(Y_b))^2) = E(Y_b)^2
//    double_vect ECXaCYaE_b;    // E((Xa - E(Xab))(Ya - E(Y_b))) = -E(Y_b)*E(Xa - E(Xab))
//    double_vect ECYaE_b2;    // E((Ya - E(Y_b))^2) = E(Y_b)^2

    ECXbCYbE_b = mean_vector(CXbCY_b);
    ECXbCYbE_b = mean_vector(CXbCY_b);
//    ECXbCYbE_b = reduce_2D_vect(CXbCY_b, mean_vector);
//    ECXbCYbE_b = reduce_2D_vect(CXbCY_b, mean_vector);
    
    ECXaCYaE_b = mean_vector(CXa);    // E(Xa - E(Xab))
    ECXaCYaE_b = mult_scalars(ECXaCYaE_b, EY_b);  // E(Y_b)*E(Xa - E(Xab))
//    ECXaCYaE_b = reduce_2D_vect(CXa, mean_vector);    // E(Xa - E(Xab))
//    ECXaCYaE_b = apply_vect_func(ECXaCYaE_b, EY_b, mult_scalars);  // E(Y_b)*E(Xa - E(Xab))
    ECXaCYaE_b = -ECXaCYaE_b;
//    for (auto& val : ECXaCYaE_b) val = -val;

    ECYbE_b2 = mean_vector(CY_b2);
    ECYaE_b2 = mult_scalars(EY_b, EY_b);  // E(Y_b)^2
//    ECYbE_b2 = reduce_2D_vect(CY_b2, mean_vector);
//    ECYaE_b2 = apply_vect_func(EY_b, EY_b, mult_scalars);  // E(Y_b)^2

    // Variance, Covariance
    double cov_X_b;
    double var_Y_b;
//    double_vect cov_X_b;
//    double_vect var_Y_b;

    cov_X_b = mean_scalars(ECXaCYaE_b, ECXbCYbE_b);
    var_Y_b = mean_scalars(ECYaE_b2, ECYbE_b2);
//    cov_X_b = apply_vect_func( ECXaCYaE_b, ECXbCYbE_b, mean_scalars);
//    var_Y_b = apply_vect_func( ECYaE_b2, ECYbE_b2, mean_scalars);

    // Correlations
    double correl_XabYab;
    double correl_XabYa_;
    double correl_XabY_b;
/***
 * TODO convert correl, rm, f, z to non-vector versions
 ***/
    correl_XabYab = cov_Xab / std::sqrt(var_Xab * var_Yab);
    correl_XabYa_ = cov_Xa_ / std::sqrt(var_Xab * var_Ya_);
    correl_XabY_b = cov_X_b / std::sqrt(var_Xab * var_Y_b);
    correl_XabYab = (correl_XabYab > 0.0 ? correl_XabYab : 0.0);
    correl_XabYa_ = (correl_XabYa_ > 0.0 ? correl_XabYa_ : 0.0);
    correl_XabY_b = (correl_XabY_b > 0.0 ? correl_XabY_b : 0.0);
//    double_vect correl_XabYab;
//    double_vect correl_XabYa_;
//    double_vect correl_XabY_b;
//    correl_XabYab = correl_vectors(cov_Xab, var_Xab, var_Yab);
//    correl_XabYa_ = correl_vectors(cov_Xa_, var_Xab, var_Ya_);
//    correl_XabY_b = correl_vectors(cov_X_b, var_Xab, var_Y_b);

    /* correlations between models */
    // Yab, Ya_ covariance...
    // Products
    double_vect CYaCYa_;    // (Ya - E(Yab))(Ya - E(Ya_))
//    double_2d CYaCYa_;    // (Ya - E(Yab))(Ya - E(Ya_))
    // (Yb - E(Yab))(Yb - E(Ya_)) --> -E(Ya_)*(Yb - E(Yab))

    CYaCYa_ = mult_vectors(CYa, CYa_);
//    CYaCYa_ = apply_vect_func(CYa, CYa_, mult_vectors);

    // region expected values
    double ECYaCYaEa_;    // E((Ya - E(Yab))(Ya - E(Ya_)))
    double ECYbCYbEa_;    // E((Yb - E(Yab))(Yb - E(Ya_))) = -E(Ya_)*E(Yb - E(Yab))
//    double_vect ECYaCYaEa_;    // E((Ya - E(Yab))(Ya - E(Ya_)))
//    double_vect ECYbCYbEa_;    // E((Yb - E(Yab))(Yb - E(Ya_))) = -E(Ya_)*E(Yb - E(Yab))

    ECYaCYaEa_ = mean_vector(CYaCYa_);
    ECYbCYbEa_ = mean_vector(CYa);    // E(Ya - E(Yab))
    ECYbCYbEa_ = mult_scalars(ECYbCYbEa_, EYa_);  // E(Ya_)*E(Ya - E(Yab))
    ECYbCYbEa_ = -ECYbCYbEa_;  // -E(Ya_)*E(Ya - E(Yab))
//    ECYaCYaEa_ = reduce_2D_vect(CYaCYa_, mean_vector);
//    ECYbCYbEa_ = reduce_2D_vect(CYa, mean_vector);    // E(Ya - E(Yab))
//    ECYbCYbEa_ = apply_vect_func(ECYbCYbEa_, EYa_, mult_scalars);  // E(Ya_)*E(Ya - E(Yab))
//    for (auto& val : ECYbCYbEa_) val = -val;  // -E(Ya_)*E(Ya - E(Yab))

    // Variance, Covariance
    double cov_YabYa_;
    cov_YabYa_ = mean_scalars(ECYaCYaEa_, ECYbCYbEa_);
//    double_vect cov_YabYa_;
//    cov_YabYa_ = apply_vect_func( ECYaCYaEa_, ECYbCYbEa_, mean_scalars);

    // Yab, Ya_ correlation
    double correl_YabYa_;
    correl_YabYa_ = cov_YabYa_ / std::sqrt(var_Yab * var_Ya_);
    correl_YabYa_ = (correl_YabYa_ > 0.0 ? correl_YabYa_ : 0.0);
//    double_vect correl_YabYa_;
//    correl_YabYa_ = correl_vectors(cov_YabYa_, var_Yab, var_Ya_);

    // Yab, Y_b covariance...
    // Products
    double_vect CYbCY_b;    // (Yb - E(Yab))(Yb - E(Y_b))
//    double_2d CYbCY_b;    // (Yb - E(Yab))(Yb - E(Y_b))
    // (Ya - E(Yab))(Ya - E(Y_b)) --> -E(Y_b)*(Ya - E(Yab))

    CYbCY_b = mult_vectors(CYb, CY_b);
//    CYbCY_b = apply_vect_func(CYb, CY_b, mult_vectors);

    // region expected values
    double ECYbCYbE_b;    // E((Yb - E(Yab))(Yb - E(Y_b)))
    double ECYaCYaE_b;    // E((Ya - E(Yab))(Ya - E(Y_b))) = -E(Y_b)*E(Ya - E(Yab))
//    double_vect ECYbCYbE_b;    // E((Yb - E(Yab))(Yb - E(Y_b)))
//    double_vect ECYaCYaE_b;    // E((Ya - E(Yab))(Ya - E(Y_b))) = -E(Y_b)*E(Ya - E(Yab))

    ECYbCYbE_b = mean_vector(CYbCY_b);
    ECYaCYaE_b = mean_vector(CYb);    // E(Yb - E(Yab))
    ECYaCYaE_b = mult_scalars(ECYaCYaE_b, EY_b);  // E(Y_b)*E(Yb - E(Yab))
    ECYaCYaE_b = -ECYaCYaE_b;  // -E(Y_b)*E(Yb - E(Yab))
//    ECYbCYbE_b = reduce_2D_vect(CYbCY_b, mean_vector);
//    ECYaCYaE_b = reduce_2D_vect(CYb, mean_vector);    // E(Yb - E(Yab))
//    ECYaCYaE_b = apply_vect_func(ECYaCYaE_b, EY_b, mult_scalars);  // E(Y_b)*E(Yb - E(Yab))
//    for (auto& val : ECYaCYaE_b) val = -val;  // -E(Y_b)*E(Yb - E(Yab))

    // Variance, Covariance
    double cov_YabY_b;
//    double_vect cov_YabY_b;

    cov_YabY_b = mean_scalars(ECYaCYaE_b, ECYbCYbE_b);
//    cov_YabY_b = apply_vect_func( ECYaCYaE_b, ECYbCYbE_b, mean_scalars);

    // Yab, Y_b correlation
    double correl_YabY_b;
    correl_YabY_b = cov_YabY_b / std::sqrt(var_Yab * var_Y_b);
    correl_YabY_b = (correl_YabY_b > 0.0 ? correl_YabY_b : 0.0);
//    double_vect correl_YabY_b;
//    correl_YabY_b = correl_vectors(cov_YabY_b, var_Yab, var_Y_b);

    // Compare correlations
    double rm2ABA0;
    double rm2AB0B;
//    double_vect rm2ABA0;
//    double_vect rm2AB0B;
    //double_vect rm2AB1r;

    // Calculate rm values between correlations
    rm2ABA0 = 0.5 * (correl_XabYab * correl_XabYab + correl_XabYa_ * correl_XabYa_);
    rm2AB0B = 0.5 * (correl_XabYab * correl_XabYab + correl_XabY_b * correl_XabY_b);
    //rm2AB1r = rm_vectors(correlAB, correl1r);

    double fABA0;
    double fAB0B;
//    double_vect fABA0;
//    double_vect fAB0B;
    //double_vect fAB1r;

    // Calculate f values between correlation and rm
    fABA0 = (1.0 - correl_XabYa_) / (2.0 * (1.0 - rm2ABA0));
    fAB0B = (1.0 - correl_XabY_b) / (2.0 * (1.0 - rm2AB0B));
//    fABA0 = f_vectors(correl_XabYa_, rm2ABA0);
//    fAB0B = f_vectors(correl_XabY_b, rm2AB0B);
    //fAB1r = f_vectors(correlAB1r, rm2AB1r);

//    double_vect hABA0;
//    double_vect hAB0B;
    double hABA0;
    double hAB0B;
    //double_vect hAB1r;

    // Calculate h values between f and rm
    hABA0 = (1.0 - fABA0 * rm2ABA0) / (1.0 - rm2ABA0);
    hAB0B = (1.0 - fAB0B * rm2AB0B) / (1.0 - rm2AB0B);
//    hABA0 = h_vectors(fABA0, rm2ABA0);
//    hAB0B = h_vectors(fAB0B, rm2AB0B);
    //hAB1r = h_vectors(fAB1r, rm2AB1r);

    // Subtract 3 and square root
//    std::for_each(nAB.begin(), nAB.end(), [](double& d) { d-=3.0;});
//    std::transform(nAB.begin(), nAB.end(), nAB.begin(),
//                                                 (double(*)(double)) sqrt);
    nAB = std::sqrt(nAB - 3.0);

    double zABA0;
    double zAB0B;
//    double_vect zABA0;
//    double_vect zAB0B;
    //double_vect zAB1r;

    // Calculate z scores
    zABA0 = (std::atanh(correl_XabYab) - std::atanh(correl_XabYa_)) * nAB / (2.0 * (1.0 - correl_YabYa_) * hABA0);
    zAB0B = (std::atanh(correl_XabYab) - std::atanh(correl_XabY_b)) * nAB / (2.0 * (1.0 - correl_YabY_b) * hAB0B);
//    zABA0 = z_vectors(correl_XabYab, correl_XabYa_, nAB, correl_YabYa_, hABA0);
//    zAB0B = z_vectors(correl_XabYab, correl_XabY_b, nAB, correl_YabY_b, hAB0B);
    //zAB1r = z_vectors(correlAB, correl1r, nAB, correlAB1r, hAB1r);

    double min_score;
//    double_vect min_score;

    // Find the minimum scores, bounded at zero
//    for (size_t idx = 0; idx < zABA0.size(); ++idx) {
//        double zA0 = zABA0[idx];
//        double z0B = zAB0B[idx];
//        //double z1r = zAB1r[idx];
//        //double min  = std::min({zA0, zB0, z1r});
//        double min  = std::min({zA0, z0B});
//        min_score.push_back(std::max({0.0, min}));
//    }
    min_score = std::max({0.0, std::min({zABA0, zAB0B})});

    // Package return values
    // double_2d score = {min_score, correlAB, correlA0, correlB0, correl1r};
    // double_2d score = {min_score, {0.0}, {0.0}, {0.0}, {0.0}};

      /* TODO: collect min_score */
    } 
//    return min_score;
//    return min_score_vect;
}
