#include "soc.h"
#include "app_cfg.h"

#include <stddef.h>

/*------------------------------------------------------------------
 * SOC：安时积分 + OCV 校正
 *----------------------------------------------------------------*/

static float    g_soc        = 100.0f;
static float    g_charged_ah  = 0.0f;    /* 累计充入安时（用于 SOH） */
static float    g_discharged_ah = 0.0f;  /* 累计放出安时 */

/* 16 串磷酸铁锂的 OCV-SOC 表：单节电压(V) -> SOC(%) */
static const float g_ocv_v[11] =
{
    3.000f, 3.150f, 3.220f, 3.260f, 3.290f,
    3.310f, 3.320f, 3.330f, 3.350f, 3.400f, 3.450f
};

static const float g_ocv_soc[11] =
{
    0.0f,  5.0f,  10.0f, 20.0f, 35.0f,
    50.0f, 60.0f, 70.0f, 85.0f, 95.0f, 100.0f
};

void soc_init(float init_soc)
{
    if (init_soc < 0.0f)
    {
        init_soc = 0.0f;
    }
    if (init_soc > 100.0f)
    {
        init_soc = 100.0f;
    }

    g_soc = init_soc;
}

void soc_integrate(float curr_a, float dt_s)
{
    float delta_ah;

    if (dt_s <= 0.0f)
    {
        return;
    }

    /* 电流符号：正 = 充电（SOC 增），负 = 放电 */
    delta_ah = curr_a * dt_s / 3600.0f;

    if (delta_ah > 0.0f)
    {
        g_charged_ah += delta_ah;
    }
    else
    {
        g_discharged_ah += -delta_ah;
    }

    /* 库仑效率：充电时只有 98% 真正存进去，放电按 100% 算 */
    {
        float eff = (delta_ah > 0.0f) ? 0.98f : 1.0f;

        g_soc += (delta_ah * eff / BAT_CAPACITY_AH) * 100.0f;
    }

    if (g_soc > 100.0f)
    {
        g_soc = 100.0f;
    }
    if (g_soc < 0.0f)
    {
        g_soc = 0.0f;
    }
}

/****************************************************************
 * OCV 校正（静置时）
 *
 * 静置时间不够的话端电压还带着极化电压，读出来偏高（充电后）
 * 或偏低（放电后），所以这只在 |I|<0.2A 的时候调，
 * 而且每次只把 SOC 拉 5%，避免一次错误读数把积分结果带跑。
 ****************************************************************/
void soc_ocv_correct(float pack_volt)
{
    float cell_v;
    float ocv_soc = 0.0f;
    float diff;

    cell_v = pack_volt / (float)BAT_CELL_SERIES;

    if (cell_v <= g_ocv_v[0])
    {
        ocv_soc = 0.0f;
    }
    else if (cell_v >= g_ocv_v[10])
    {
        ocv_soc = 100.0f;
    }
    else
    {
        for (uint8_t i = 0; i < 10; i++)
        {
            if (cell_v >= g_ocv_v[i] && cell_v < g_ocv_v[i + 1])
            {
                float ratio = (cell_v - g_ocv_v[i]) / (g_ocv_v[i + 1] - g_ocv_v[i]);

                ocv_soc = g_ocv_soc[i] + ratio * (g_ocv_soc[i + 1] - g_ocv_soc[i]);
                break;
            }
        }
    }

    diff = ocv_soc - g_soc;

    /* 只校 5%，而且差异小于 2% 就不动，避免平台段来回拉扯 */
    if (diff > 2.0f || diff < -2.0f)
    {
        g_soc += diff * 0.05f;

        if (g_soc > 100.0f)
        {
            g_soc = 100.0f;
        }
        if (g_soc < 0.0f)
        {
            g_soc = 0.0f;
        }
    }
}

float soc_get(void)
{
    return g_soc;
}

uint16_t soc_get_10(void)
{
    return (uint16_t)(g_soc * 10.0f + 0.5f);
}

/****************************************************************
 * SOH：简化算法
 *
 * 真正的 SOH 要测内阻或者做一次完整充放电看实际容量，
 * 这里用"累计安时 / 额定安时"来体现电池被用掉多少循环，
 * 只作为趋势参考，不能当精确值。
 ****************************************************************/
uint16_t soh_get_10(void)
{
    float cycles;
    float soh;

    cycles = (g_charged_ah + g_discharged_ah) / (2.0f * BAT_CAPACITY_AH);

    if (cycles < 1.0f)
    {
        cycles = 1.0f;
    }

    /* 按 3000 次循环衰减到 80% 线性估算 */
    soh = 100.0f - (cycles / 3000.0f) * 20.0f;

    if (soh < 80.0f)
    {
        soh = 80.0f;
    }

    return (uint16_t)(soh * 10.0f + 0.5f);
}
