/*
The MIT License (MIT)

Copyright (c) 2015-? suhetao

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "stm32f4_mpu9250.h"
#include "stm32f4_serial.h"
#include "Quaternion.h"

#include "stm32f4_delay.h"
#include "stm32f4_rcc.h"
#include "stm32f4_exti.h"

#include "inv_mpu.h"
#include "inv_mpu_dmp_motion_driver.h"

#define DEFAULT_MPU_HZ  (200)
#define GYRO_TORAD(x) (((float)(x)) * 0.00106422515365507901031932363932f)

//////////////////////////////////////////////////////////////////////////
//uncomment one
//#define USE_EKF
//#define USE_UKF
//#define USE_CKF
#define USE_SRCKF
//#define USE_9AXIS_EKF

//for doctor's mini Quadrotor
//#define USE_6AXIS_EKF
//#define USE_6AXIS_FP_EKF

//////////////////////////////////////////////////////////////////////////

#ifdef USE_EKF
#include "EKF.h"
#elif defined USE_UKF
#include "UKF.h"
#elif defined USE_CKF
#include "CKF.h"
#elif defined USE_SRCKF
#include "SRCKF.H"
#elif defined USE_6AXIS_EKF
#include "miniIMU.h"
#elif defined USE_6AXIS_FP_EKF
#include "FP_miniIMU.h"
#elif defined USE_9AXIS_EKF
#include "miniAHRS.h"
#endif

static s8 gyro_orientation[9] = {
	1, 0, 0,
	0, 1, 0,
	0, 0, 1
};

static __inline unsigned short inv_row_2_scale(const signed char *row)
{
    unsigned short b;
    if (row[0] > 0)
        b = 0;
    else if (row[0] < 0)
        b = 4;
    else if (row[1] > 0)
        b = 1;
    else if (row[1] < 0)
        b = 5;
    else if (row[2] > 0)
        b = 2;
    else if (row[2] < 0)
        b = 6;
    else
        b = 7;      // error
    return b;
}

static __inline unsigned short inv_orientation_matrix_to_scalar(const signed char *mtx){
    unsigned short scalar;
    /*
       XYZ  010_001_000 Identity Matrix
       XZY  001_010_000
       YXZ  010_000_001
       YZX  000_010_001
       ZXY  001_000_010
       ZYX  000_001_010
     */
    scalar = inv_row_2_scale(mtx);
    scalar |= inv_row_2_scale(mtx + 3) << 3;
    scalar |= inv_row_2_scale(mtx + 6) << 6;
    return scalar;
}

int main(void)
{
	//PLL_M PLL_N PLL_P PLL_Q
	PLL_PARAMS pFreq120M = {12, 240, 2, 5};
	//PLL_PARAMS pFreq128M = {12, 256, 2, 6};
	//PLL_PARAMS pFreq168M = {12, 336, 2, 7};

	s32 s32Result = 0;
	struct int_param_s pInitParam = {0};
	u8 u8AccelFsr = 0;
	u16 u16GyroRate = 0;
	u16 u16GyroFsr = 0;
	//u16 u16DmpFeatures = DMP_FEATURE_6X_LP_QUAT | DMP_FEATURE_SEND_RAW_ACCEL | DMP_FEATURE_SEND_CAL_GYRO |
		//DMP_FEATURE_GYRO_CAL;
	u16 u16DmpFeatures = DMP_FEATURE_6X_LP_QUAT | DMP_FEATURE_SEND_RAW_ACCEL | DMP_FEATURE_SEND_CAL_GYRO;

	s16 s16Gyro[3] = {0}, s16Accel[3] = {0}, s16Mag[3] = {0};
	float fRealGyro[3] = {0}, fRealAccel[3] = {0};
#if !defined USE_6AXIS_EKF && !defined USE_6AXIS_FP_EKF
	float fRealMag[3] = {0}, fRealQ[4] = {0};
#endif
	s16 s16Sensors = 0;
	u8 u8More = 0;
	long lQuat[4] = {0};
	unsigned long ulTimeStamp = 0;
	float fRPY[3] = {0};
	float fQ[4] = {0};

#ifdef USE_EKF
	EKF_Filter ekf;
#elif defined USE_UKF
	UKF_Filter ukf;
#elif defined USE_CKF
	CKF_Filter ckf;
#elif defined USE_SRCKF
	SRCKF_Filter srckf;
#endif

	unsigned long ulNowTime = 0;
	unsigned long ulLastTime = 0;
	unsigned long ulSendTime = 0;
	float fDeltaTime = 1.0f;
	u32 u32KFState = 0;
	
	//Reduced frequency
	//128 / 4 = 32Mhz APB1, 32/32 = 1MHz SPI Clock
	//1Mhz SPI Clock for read/write
	RCC_SystemCoreClockUpdate(pFreq120M);
	Delay_Init();
	MPU9250_Init();
	
#ifdef USE_EKF
	//Create a new EKF object;
	EKF_New(&ekf);
#elif defined USE_UKF
	//Create a new UKF object;
	UKF_New(&ukf);
#elif defined USE_CKF
	//Create a new CKF object;
	CKF_New(&ckf);
#elif defined USE_SRCKF
	SRCKF_New(&srckf);
#endif
	//////////////////////////////////////////////////////////////////////////
	//Init DMP
	s32Result = mpu_init(&pInitParam);
	s32Result = mpu_set_sensors(INV_XYZ_GYRO | INV_XYZ_ACCEL | INV_XYZ_COMPASS);
	s32Result = mpu_configure_fifo(INV_XYZ_GYRO | INV_XYZ_ACCEL);
	s32Result = mpu_set_sample_rate(DEFAULT_MPU_HZ);
	s32Result = mpu_get_sample_rate(&u16GyroRate);
	s32Result = mpu_get_gyro_fsr(&u16GyroFsr);
	s32Result = mpu_get_accel_fsr(&u8AccelFsr);
	s32Result = dmp_load_motion_driver_firmware();
	//
	s32Result = dmp_set_orientation(inv_orientation_matrix_to_scalar(gyro_orientation));
	//
	s32Result = dmp_enable_feature(u16DmpFeatures);
	s32Result = dmp_set_fifo_rate(DEFAULT_MPU_HZ);
	s32Result = mpu_set_dmp_state(1);
	//////////////////////////////////////////////////////////////////////////
	//Recover frequency
	//why DMP fifo must be reset when it overflows.
	//SPI write operation occur, when you reset DMP fifo,
	//but it can' write at 20Mhz SPI Clock? Fix me!
#if 0
	RCC_SystemCoreClockUpdate(pFreq168M);
	Delay_Init();
	MPU9250_Init();
	//42Mhz APB1, 42/2 = 21 MHz SPI Clock
	MPU9250_SPIx_SetDivisor(SPI_BaudRatePrescaler_2);
#endif
	Interrupt_Init();
	Serial_Init();

	for(;;){
		if (Interrupt_GetState()){
			s32Result = dmp_read_fifo(s16Gyro, s16Accel, lQuat, &ulTimeStamp, &s16Sensors, &u8More);
			if(s32Result < 0){
				continue;
			}
			//because 20Mhz SPI Clock satisfy MPU9250 with read condition
			//so that you can't use I2C Master mode read/write from SPI at 20Mhz SPI Clock
			//and dmp fifo can't use for Magnetometer, but you can use this function below
			s32Result = mpu_get_compass_reg(s16Mag, &ulTimeStamp);
			//must calibrate gryo, accel, magnetic data
			//ned coordinate system
			//todo
			fRealGyro[0] = GYRO_TORAD(s16Gyro[0]);
			fRealGyro[1] = GYRO_TORAD(s16Gyro[1]);
			fRealGyro[2] = -GYRO_TORAD(s16Gyro[2]);

			fRealAccel[0] = s16Accel[0] / 16384.0f;
			fRealAccel[1] = s16Accel[1] / 16384.0f;
			fRealAccel[2] = s16Accel[2] / 16384.0f;
#if !defined USE_6AXIS_EKF && !defined USE_6AXIS_FP_EKF
			if(!s32Result){
				fRealMag[0] = s16Mag[0];
				fRealMag[1] = s16Mag[1];
				fRealMag[2] = s16Mag[2];
			}

			//q30 to float
			fRealQ[0] = (float)lQuat[0] / 1073741824.0f;
			fRealQ[1] = (float)lQuat[1] / 1073741824.0f;
			fRealQ[2] = (float)lQuat[2] / 1073741824.0f;
			fRealQ[3] = (float)lQuat[3] / 1073741824.0f;
#endif
			////
			Get_Ms(&ulNowTime);
			if(!u32KFState){
				if(s32Result < 0){
					continue;
				}
#ifdef USE_EKF
				EKF_Init(&ekf, fRealQ, fRealGyro);
#elif defined USE_UKF
				UKF_Init(&ukf, fRealQ, fRealGyro);
#elif defined USE_CKF
				CKF_Init(&ckf, fRealQ, fRealGyro);
#elif defined USE_SRCKF
				SRCKF_Init(&srckf, fRealAccel, fRealMag);
#elif defined USE_6AXIS_EKF
				EKF_IMUInit(fRealAccel, fRealGyro);
#elif defined USE_6AXIS_FP_EKF
				FP_EKF_IMUInit(fRealAccel, fRealGyro);
#elif defined USE_9AXIS_EKF
				EKF_AHRSInit(fRealAccel, fRealMag);
#endif				
				ulLastTime = ulNowTime;
				ulSendTime = ulNowTime;
				u32KFState = 1;
			}
			else{
				fDeltaTime = 0.001f * (float)(ulNowTime - ulLastTime);
#ifdef USE_EKF
				EFK_Update(&ekf, fRealQ, fRealGyro, fRealAccel, fRealMag, fDeltaTime);
#elif defined USE_UKF
				UKF_Update(&ukf, fRealQ, fRealGyro, fRealAccel, fRealMag, fDeltaTime);
#elif defined USE_CKF
				CKF_Update(&ckf, fRealQ, fRealGyro, fRealAccel, fRealMag, fDeltaTime);
#elif defined USE_SRCKF
				SRCKF_Update(&srckf, fRealGyro, fRealAccel, fRealMag, fDeltaTime);
#elif defined USE_6AXIS_EKF
				EKF_IMUUpdate(fRealGyro, fRealAccel, fDeltaTime);
#elif defined USE_6AXIS_FP_EKF
				FP_EKF_IMUUpdate(fRealGyro, fRealAccel, fDeltaTime);
#elif defined USE_9AXIS_EKF
				EKF_AHRSUpdate(fRealGyro, fRealAccel, fRealMag, fDeltaTime);
#endif	
			}
			
#ifdef USE_EKF
			EKF_GetAngle(&ekf, fRPY);
			EKF_GetQ(&ekf, fQ);
#elif defined USE_UKF
			UKF_GetAngle(&ukf, fRPY);
			UKF_GetQ(&ukf, fQ);
#elif defined USE_CKF
			CKF_GetAngle(&ckf, fRPY);
			CKF_GetQ(&ckf, fQ);
#elif defined USE_SRCKF
			SRCKF_GetAngle(&srckf, fRPY);
			SRCKF_GetQ(&srckf, fQ);
#elif defined USE_6AXIS_EKF
			EKF_IMUGetAngle(fRPY);
			EKF_IMUGetQ(fQ);
#elif defined USE_6AXIS_FP_EKF
			FP_EKF_IMUGetAngle(fRPY);
			FP_EKF_IMUGetQ(fQ);
#elif defined USE_9AXIS_EKF
			EKF_AHRSGetAngle(fRPY);
			EKF_AHRSGetQ(fQ);
#endif
			//transmit Quaternion float format to Q31
			lQuat[0] = (long)(fQ[0] * 2147483648.0f);
			lQuat[1] = (long)(fQ[1] * 2147483648.0f);
			lQuat[2] = (long)(fQ[2] * 2147483648.0f);
			lQuat[3] = (long)(fQ[3] * 2147483648.0f);
			//todo
			//transmit the gyro, accel, mag, quat roll pitch yaw to anywhere
			//1000 / 10 = 100HZ
			if(ulNowTime - ulSendTime > 9){
				Serial_Upload(s16Accel, s16Gyro, s16Mag, lQuat, 0, 0);
				ulSendTime = ulNowTime;
			}
			ulLastTime = ulNowTime; 
		}
	}
}
