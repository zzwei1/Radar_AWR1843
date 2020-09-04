
#include "../common/oddemo_common.h"
#include <math.h>
#include <float.h>
#include <oddemo_feature.h>

//This is the normal amount of power in an empty zone
#define ODDEMO_POWER_INIT_VAL          48.0

//Feature extraction global variables
ODDEMO_Feature oddemo_feature[ODDEMO_ZONE_PAIR];
ODDEMO_Zone    oddemo_zone[ODDEMO_MAX_ZONES];
ODDEMO_Parms   oddemo_parms;
uint16_t       oddemo_num_zones = 0;
uint16_t       oddemo_zone_pairs = 0;
float          oddemo_meanVec[ODDEMO_ZONE_PAIR][ODDEMO_MATRIX_ROW_SIZE-1];
float          oddemo_stdVec[ODDEMO_ZONE_PAIR][ODDEMO_MATRIX_ROW_SIZE-1];

uint16_t       ph_idx;
float          power_history[ODDEMO_MAX_ZONES][ODDEMO_MAX_FRAME_HIST];
float          power_historydB[ODDEMO_MAX_ZONES][ODDEMO_MAX_FRAME_HIST];

extern ODDEMO_Decision oddemo_decision[ODDEMO_MAX_ZONES];


float DSPF_sp_vecsum_sq(const float * x, const int n)
{
    int i;
    float sum = 0;

//    _nassert(n > 0);
//    _nassert((int)x % 8 == 0);

    /* Unroll by 8 because the loop carried dependency bound is 4 */
    /* The compiler does not do staggered adds                    */
    #pragma UNROLL(4)
    for(i = 0; i < n; i++)
        sum += x[i] * x[i];

    return sum;
}


float DSPF_sp_vecavg(float * x, int n)
{
    int i;
    float sum = 0;

//    _nassert(n > 0);
//    _nassert((int)x % 8 == 0);

    /* Unroll by 8 because the loop carried dependency bound is 4 */
    /* The compiler does not do staggered adds                    */
    #pragma UNROLL(4)
    for(i = 0; i < n; i++)
        sum += x[i];

    sum = sum/n;
    return sum;
}


float DSPF_sp_vecvar(const float * x, const int n)
{
    int i;
    float sum = 0, avg = 0, var = 0;

//    _nassert(n > 0);
//    _nassert((int)x % 8 == 0);

    /* Unroll by 8 because the loop carried dependency bound is 4 */
    /* The compiler does not do staggered adds                    */
    #pragma UNROLL(4)
    for(i = 0; i < n; i++)
        sum += x[i];

    avg = sum/n;

    /* Unroll by 8 because the loop carried dependency bound is 4 */
    /* The compiler does not do staggered adds                    */
    #pragma UNROLL(4)
    for(i = 0; i < n; i++)
        var += (x[i]-avg) * (x[i]-avg);
    return (var / (n - 1));
}


float DSPF_sp_sqrtsp (float a)
{
  const float  Half  = 0.5f;
  const float  OneP5 = 1.5f;
  float  x, y;
  int i;

  x = _rsqrsp(a);

  #pragma UNROLL(1)
  for(i=0; i< 2; i++){
    x = x * (OneP5 - (a * x * x * Half));
  }
  y = a * x;

  if (a <= 0.0f) {
    y = 0.0f;
  }
  if (a > FLT_MAX) {
    y = FLT_MAX;
  }

  return (y);
}


float DSPF_sp_crosscorr(const float *x,
                        const float *y,
                        const float  sqRtVarx,
                        const float  sqRtVary,
                        const int    n)
{
    int   i;
    float tmp;
    float sumx = 0, sumy = 0, avgx = 0, avgy = 0, crosscor = 0;

//    _nassert(n > 0);
//    _nassert((int)x % 8 == 0);

    /* Unroll by 8 because the loop carried dependency bound is 4 */
    /* The compiler does not do staggered adds                    */
    #pragma UNROLL(4)
    for (i = 0; i < n; i++)
    {
        sumx += x[i];
        sumy += y[i];
    }
    avgx = sumx/n;
    avgy = sumy/n;

    /* Unroll by 8 because the loop carried dependency bound is 4 */
    /* The compiler does not do staggered adds                    */
    #pragma UNROLL(4)
    for (i = 0; i < n; i++)
        crosscor += (x[i]-avgx) * (y[i]-avgy);

    tmp = sqRtVarx * sqRtVary * (n - 1);

    crosscor = crosscor / tmp;

    return crosscor;
}


void ODDemo_Feature_init(void)
{
    uint16_t idx, idx2;
    float  init_power, log_power;

    //This variable always indicates the oldest entry in the power history buffers.
    ph_idx = 0;
    oddemo_num_zones = 0;

    init_power = ODDEMO_POWER_INIT_VAL;
    log_power  = 10 * log10(init_power);

    for (idx = 0; idx < ODDEMO_MAX_ZONES; idx++)
    {
      for (idx2 = 0; idx2 < ODDEMO_MAX_FRAME_HIST; idx2++)
      {
        power_history[idx][idx2] = init_power;
        power_historydB[idx][idx2] = log_power;
      }
    }
}


void ODDemo_Feature_extract(uint16_t        zone_pair,
                            float          *azimuthHeatMap,
                            ODDEMO_Feature *feature)
{
    uint16_t idx, az_idx, rng_idx;
    uint16_t max_az, max_rng, offset;
    uint16_t zone;

    float *heat;
    float  sum, max;
    float  tmp;
//    float  size;
    float  var;
    float  crossCorr;
    float  avgPower;
    float  powerMA[2];
    float  powerMAdB[2];
    float  powRatio[2];
    float  powRatiodB[2];
    float  sqRootVar[2];


    // (Step 1) Compute Average Power for each zone
    // (this assumes zone sizes have been checked and clipped)
    zone = zone_pair * 2;

    for (idx = 0; idx < 2; idx ++)
    {
#ifdef FULL_ZONE_SUM
        sum  = 0;
        size = (float)(oddemo_zone[zone].range_length * oddemo_zone[zone].azimuth_width);

        for (rng_idx = 0; rng_idx < oddemo_zone[zone].range_length; rng_idx++)
        {
            // point to the correct range row, then add the starting azimuth index
            heat  = azimuthHeatMap + (ODDEMO_MAX_AZIMUTH * (oddemo_zone[zone].range_start + rng_idx));
            heat += oddemo_zone[zone].azimuth_start;

            for (az_idx = 0; az_idx < oddemo_zone[zone].azimuth_width; az_idx++)
                sum += *heat++;
        }

        avgPower = sum / size;

#else //do 5x5 cell power sums

        //find the max 3x3 area inside the zone
        max   = 0;
        heat  = azimuthHeatMap + (ODDEMO_MAX_AZIMUTH * oddemo_zone[zone].range_start);
        heat += oddemo_zone[zone].azimuth_start;

        for (rng_idx = 0; rng_idx < oddemo_zone[zone].range_length-2; rng_idx++)
        {
            // point to the correct range row, then add the starting azimuth index
            for (az_idx = 0; az_idx < oddemo_zone[zone].azimuth_width-2; az_idx++)
            {
              offset = az_idx;
              tmp  = heat[offset] + heat[offset+1] + heat[offset+2];
              offset += ODDEMO_MAX_AZIMUTH;
              tmp += heat[offset] + heat[offset+1] + heat[offset+2];
              offset += ODDEMO_MAX_AZIMUTH;
              tmp += heat[offset] + heat[offset+1] + heat[offset+2];

              if (tmp > max)
              {
                max = tmp;
                max_rng = rng_idx; //center position is actually +1, +1
                max_az  = az_idx;
              }
            }

            heat += ODDEMO_MAX_AZIMUTH;
        }

       // Save the max positions for output.
        max_rng += oddemo_zone[zone].range_start + 1;
        max_az  += oddemo_zone[zone].azimuth_start + 1;
        oddemo_zone[zone].range_max_idx = max_rng;
        oddemo_zone[zone].azimuth_max_idx = max_az;

        sum   = 0;
        heat  = azimuthHeatMap + (ODDEMO_MAX_AZIMUTH * (max_rng-2));
        heat += max_az - 2;

        //compute average power on a 5x5 centered on the max 3x3
        for (rng_idx = 0; rng_idx < 5; rng_idx++)
        {
            for (az_idx = 0; az_idx < 5; az_idx++)
            {
                sum += heat[az_idx];
            }

            heat += ODDEMO_MAX_AZIMUTH;
        }

        avgPower = sum / 25; //fixed 25 cell size
#endif

        // (Step 2) store new average power, overwriting the oldest value in the history buffer
        power_history[zone][ph_idx] = avgPower;
        power_historydB[zone][ph_idx] = 10 * log10(avgPower);

        // (Step 3) Moving Average of previous average power values
        powerMA[idx] = DSPF_sp_vecavg(&power_history[zone][0], oddemo_parms.windowLen);
        powerMAdB[idx] = 10 * log10(powerMA[idx]);

        //this value is for output only:
        if (powerMA[idx] > 999999.9)
          oddemo_decision[zone].avgPower = 999999.9;
        else if (powerMA[idx] < 0.0)
          oddemo_decision[zone].avgPower = 0.0;
        else
          oddemo_decision[zone].avgPower = powerMA[idx];

        // (Step4) Cross Correlation between the zones
        // (Step4a) Compute Variance for each zone

        var = DSPF_sp_vecvar(&power_historydB[zone][0], oddemo_parms.windowLen);

        // (Step4b) Compute Square Root of Variance
        sqRootVar[idx] = DSPF_sp_sqrtsp(var);
        zone ++;
    }

    zone = zone_pair * 2;

    //update the history index for the next frame
    ph_idx ++;
    if (ph_idx == oddemo_parms.windowLen)
      ph_idx = 0;

    // (Step 5) Compute Cross Correlation Coefficient
    // (currently hardcoded for two zones)
    crossCorr = DSPF_sp_crosscorr(&power_historydB[zone][0],
                                  &power_historydB[zone+1][0],
                                  sqRootVar[0],
                                  sqRootVar[1],
                                  oddemo_parms.windowLen);

    // (Step 6) Compute Power Ratios
    sum = 0.0;
    for (idx = 0; idx < 2; idx ++)
      sum += powerMA[idx];

    if (sum != 0.0)
    {
        for (idx = 0; idx < 2; idx ++)
        {
          powRatio[idx] = powerMA[idx] / sum;
          powRatiodB[idx] = 10 * log10(powRatio[idx]);
        }
    }
    else
    {
        for (idx = 0; idx < 2; idx ++)
        {
          powRatio[idx] = 0.0;
          powRatiodB[idx] = 0.0;
        }
    }

    // (Step 7) Save features to the normalized Feature Vector
    feature->powerMA[0]  = (powerMAdB[0] - oddemo_meanVec[zone_pair][0]) / oddemo_stdVec[zone_pair][0];
    feature->powerMA[1]  = (powerMAdB[1] - oddemo_meanVec[zone_pair][1]) / oddemo_stdVec[zone_pair][1];
    feature->powRatio[0] = (powRatiodB[0]- oddemo_meanVec[zone_pair][2]) / oddemo_stdVec[zone_pair][2];
    feature->powRatio[1] = (powRatiodB[1]- oddemo_meanVec[zone_pair][3]) / oddemo_stdVec[zone_pair][3];
    feature->crossCorr   = (crossCorr    - oddemo_meanVec[zone_pair][4]) / oddemo_stdVec[zone_pair][4];
}
