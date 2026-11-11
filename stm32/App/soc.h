#ifndef __SOC_H__
#define __SOC_H__

#include <stdint.h>

/*------------------------------------------------------------------
 * SOC 估算
 *
 * 两个手段结合：
 *   1) 安时积分（主）：SOC += ∫I·dt / 额定容量
 *      优点是实时性好，缺点是长期会漂（传感器零漂会被积分放大）
 *   2) OCV 校正（辅）：静置时端电压和 SOC 有比较稳定的对应关系，
 *      拿它把积分结果拉回来
 *
 * OCV-SOC 用一张 16 串磷酸铁锂的典型表（充放电平台很平，
 * 3.2~3.3V 之间对应 30%~90%，所以中间段靠积分、两头靠 OCV）。
 *----------------------------------------------------------------*/

void  soc_init(float init_soc);
void  soc_integrate(float curr_a, float dt_s);
void  soc_ocv_correct(float pack_volt);

float soc_get(void);
uint16_t soc_get_10(void);

/* SOH 简化估算：用累计安时数跟额定容量的比值，没有做内阻测试 */
uint16_t soh_get_10(void);

#endif
