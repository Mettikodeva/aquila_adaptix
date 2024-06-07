#include "Arduino.h"
#include "servo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pid.h"
#include "VL53L0X.h"
#include "pca9548a.h"
#include "freertos/queue.h"
#include "helper_3dmath.h"
#include <inttypes.h>
#include "iot_button.h"
#include "headers.h"

// MARK: MACROS
// adjust these values to match the servo's calibration values

#define MAP(x,in_min,in_max,out_min,out_max) (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min

#define PWM1(x) MAP(x, 0, 90, 2370, 1050)
#define PWM2(x) MAP(x, 0, 90, 2460, 1090)
#define PWM3(x) MAP(x, 0, 90, 2500, 1070)


// MARK: Variables
Servo *servo = new Servo(20);

TaskHandle_t TaskHandle_IMU;
TaskHandle_t TaskHandle_initServo;
TaskHandle_t TaskHandle_sweepServo;
TaskHandle_t TaskHandle_lidar;
TaskHandle_t TaskHandle_lidarControl;
TaskHandle_t TaskHandle_imuControl;
TaskHandle_t TaskHandle_servoWrite;
TaskHandle_t TaskHandle_suspended;
TaskHandle_t TaskHandle_terrain;
TaskHandle_t TaskHandle_ADC;

SemaphoreHandle_t servo_sem = xSemaphoreCreateBinary();
SemaphoreHandle_t legs_fk_sem = xSemaphoreCreateBinary();
// portMUX_TYPE terrainMux = portMUX_INITIALIZER_UNLOCKED;
// portMUX_TYPE servoMux = portMUX_INITIALIZER_UNLOCKED;


static const char *TAG = "lg";
static const char *TAG_LOG = "LOG";
static const char *TAG_BUTTON = "button";
// IMU
extern void imuTask(void *);
extern float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector
float lidar_pr[2]; // pitch, roll
float prev_ypr[3];
extern SemaphoreHandle_t imu_mutex;
// END IMU

// ADC
extern void startAdcTask(void *);
extern float getAngle(int8_t servo);
extern int16_t adc_value_filtered[3];
extern int16_t adc_value[3];
// END ADC

// CONTROL //pcval = pitch control value, rcval = roll control value
float kpl = (int)CONFIG_KP_VAL_LIDAR / 100.0f;
float kil = (int)CONFIG_KI_VAL_LIDAR / 100.0f;
float kdl = (int)CONFIG_KD_VAL_LIDAR / 100.0f;
// float kpl = 0.5;
// float kil = 0;
// float kdl = 0.001;
#ifdef CONFIG_INT_SAT_LIDAR_VAL
float IntSatL = CONFIG_INT_SAT_LIDAR_VAL;
#endif

float kpi = (int)CONFIG_KP_VAL_IMU / 100.0f;
float kii = (int)CONFIG_KI_VAL_IMU / 100.0f;
float kdi = (int)CONFIG_KD_VAL_IMU / 100.0f;
// float kpi = 0.5;
// float kii = 0.01;
// float kdi = 0.1;
#ifdef CONFIG_INT_SAT_IMU_VAL
float IntSatI = CONFIG_INT_SAT_IMU_VAL;
#endif

float m1 = 0, m2 = 0, m3 = 0, pcval_imu = 0, rcval_imu = 0, pcval_lidar = 0, rcval_lidar = 0;
SemaphoreHandle_t imu_control_mutex = xSemaphoreCreateBinary();
SemaphoreHandle_t lidar_control_mutex = xSemaphoreCreateBinary();
#ifdef CONFIG_INT_SAT_LIDAR_VAL
    PID * pid_pitch = new PID(kpl, kil, kdl, IntSatL); // lidar
    PID * pid_roll = new PID(kpl, kil, kdl, IntSatL);  // lidar 
#else
    PID * pid_roll = new PID(kpl, kil, kdl);  // lidar 
    PID * pid_pitch = new PID(kpl, kil, kdl); // lidar
#endif
#ifdef CONFIG_INT_SAT_IMU_VAL
    PID * pid_roll2 = new PID(kpi, kii, kdi, IntSatI); // imu
    PID * pid_pitch2 = new PID(kpi, kii, kdi, IntSatI); // imu
#else
    PID * pid_roll2 = new PID(kpi, kii, kdi); // imu
    PID * pid_pitch2 = new PID(kpi, kii, kdi); // imu
#endif

int base_pwm = 1200; // in microseconds
int default_angle = 30;
int prev_pwm[3] = {0, 0, 0}; // in microseconds
VectorInt16 legs[3];    // x, y, z
int pwms[3] = {1200,1200,1200};          // in microseconds
float terrain_pitch = 0, terrain_roll = 0;
int target_servo_angle_terrain[3] = {0,0,0};
char tolerance = 1; // in degrees


// END CONTROL


// LIDAR
extern VL53L0X **sensor;
extern void lidarTask(void *);
extern uint16_t distance[3];
extern PCA9548A *mux;
// END LIDAR

// main  var
// extern typedef enum
// {
// } State_LG_t;
extern State_LG_t landing_gear_state;


SemaphoreHandle_t pid_mutex = xSemaphoreCreateBinary();


// MARK: Helper
float deg2rad(float deg){
    return deg * PI / 180;
}

float rad2deg(float rad){
    return rad * 180 / PI;
}

VectorFloat getPlaneNorm(VectorInt16 a, VectorInt16 b, VectorInt16 c){
    VectorInt16 u = {int16_t(b.x - a.x), int16_t(b.y - a.y), int16_t(b.z - a.z)}; 
    VectorInt16 v = {int16_t(c.x - a.x), int16_t(c.y - a.y), int16_t(c.z - a.z)};

    // normal vector of the plane
    /*  | i   j   k |
        | u.x u.y u.z |
        | v.x v.y v.z |
        
        |u.y u.z|       |u.x u.z|       |u.x u.y|
        |v.y v.z| * i - |v.x v.z| * j + |v.x v.y| * k

        i(u.y * v.z - u.z * v.y) - j(u.x * v.z - u.z * v.x) + k(u.x * v.y - u.y * v.x)
    */

    VectorFloat n = {float(u.y * v.z - u.z * v.y), float(u.z * v.x - u.x * v.z), float(u.x * v.y - u.y * v.x)};
    
    return n.getNormalized();
}

// SECTION: -Tasks 

// MARK: -SERVO_T
void servoWriteTask(void *pvParameter){
    // printf("Servo Write Task\n");
    // xSemaphoreTakeRecursive(servo_sem, 10/portTICK_PERIOD_MS);
    // for (int i = 2000;i<base_pwm;i-=1){
    //     servo->writeMicroseconds(0,i);
    //     servo->writeMicroseconds(1,i);
    //     servo->writeMicroseconds(2,i);
    //     vTaskDelay(500 / portTICK_PERIOD_MS);
    // }
    // xSemaphoreGiveRecursive(servo_sem);
    TickType_t lastTime;
    

        lastTime = xTaskGetTickCount();
        for (;;)
        {
            vTaskDelayUntil(&lastTime, 100 / portTICK_PERIOD_MS);

            // if (RAD_TO_DEG * ypr[1] <= tolerance && RAD_TO_DEG * ypr[1] >= -tolerance && RAD_TO_DEG * ypr[0] <= tolerance && RAD_TO_DEG * ypr[0] >= -tolerance){
            //     vTaskDelayUntil(&lastTime, 20 / portTICK_PERIOD_MS);
            //     continue;
            // }
            
            // xQueueTakeMutexRecursive(imu_control_mutex, 10/portTICK_PERIOD_MS);
            // xQueueTakeMutexRecursive(lidar_control_mutex, 10/portTICK_PERIOD_MS);
            
            // m1 += 3*(-pcval_lidar) - pcval_imu - (rcval_imu + rcval_lidar);
            // m2 += 3*(-pcval_lidar) - pcval_imu + (rcval_imu + rcval_lidar);
            // m3 += 2 * (pcval_imu + pcval_lidar);
            // portENTER_CRITICAL(&servoMux);
            switch (landing_gear_state)
            {
            case TOUCHDOWN:
                
                m1 += 3*pcval_imu + 2*rcval_imu;
                m2 += 3*pcval_imu - 2*rcval_imu;
                m3 += 2*pcval_imu;
                break;
            case DESCENDING:
                if(target_servo_angle_terrain[0] == -1|| target_servo_angle_terrain[1] == -1|| target_servo_angle_terrain[2] == -1)
                    break;
                m1 = PWM1(target_servo_angle_terrain[0]);
                m2 = PWM2(target_servo_angle_terrain[1]);
                m3 = PWM3(target_servo_angle_terrain[2]);
            default:
                break;
            }
            // portEXIT_CRITICAL(&servoMux);
            // IMU only
            
            // xQueueGiveMutexRecursive(imu_control_mutex);
            // xQueueGiveMutexRecursive(lidar_control_mutex);
            vTaskDelay(10 / portTICK_PERIOD_MS);

            // ESP_LOGI("SWrite","pcl: %f, rcl: %f, pci: %f, rci: %f\n", pcval_lidar, rcval_lidar, pcval_imu, rcval_imu);

            // m1 = m1 > 400 ? 400 : m1 < -400 ? -400: m1;
            // m2 = m2 > 400 ? 400 : m2 < -400 ? -400: m2;
            // m3 = m3 > 400 ? 400 : m3 < -400 ? -400: m3;
            switch(landing_gear_state){
            case TOUCHDOWN:
                if (m1 > 500)
                {
                    m1 = 500;
                }
                else if (m1 < -500)
                {
                    m1 = -500;
                }
                if (m2 > 500)
                {
                    m2 = 500;
                }
                else if (m2 < -500)
                {
                    m2 = -500;
                }
                if (m3 > 500)
                {
                    m3 = 500;
                }
                else if (m3 < -500)
                {
                    m3 = -500;
                }
                // ESP_LOGI("SWrite","m1: %f, m2: %f, m3: %f\n", m1, m2, m3);
                pwms[0] = PWM1(default_angle) + m1;
                pwms[1] = PWM2(default_angle) + m2;
                pwms[2] = PWM3(default_angle) + m3;
                break;
            case DESCENDING:
                pwms[0] = m1;
                pwms[1] = m2;
                pwms[2] = m3;
                break;
            default:
                break;
            }

    
            servo->writeMicroseconds(0, pwms[0]);
            servo->writeMicroseconds(1, pwms[1]);
            servo->writeMicroseconds(2, pwms[2]);
    }
}

//  MARK: -IMU_T
void imuControlLoopTask(void *pvParameter){
    
    ESP_LOGI(TAG, "IMU Control Loop Task Started!");
    TickType_t prev_time;
    prev_ypr[1] = ypr[1];
    prev_ypr[2] = ypr[2];
    // char deg_per_sec_max = 90;
    
    prev_time = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&prev_time, 50 / portTICK_PERIOD_MS);
        
        // xQueueTakeMutexRecursive(imu_control_mutex, 10/portTICK_PERIOD_MS);
        if(abs(ypr[1]) < deg2rad(2) && abs(ypr[2]) < deg2rad(2)){
            pcval_imu = 0;
            rcval_imu = 0;
        }
        // else if(abs(prev_ypr[1]-ypr[1]) * 33/1000 > deg_per_sec_max || abs(prev_ypr[2]-ypr[2]) * 33/1000 > deg_per_sec_max){
        //     pcval_imu = 0;
        //     rcval_imu = 0;
        // }
        else{
            pcval_imu = pid_pitch2->update(0 - rad2deg(ypr[1]));
            rcval_imu = pid_roll2->update(0 - rad2deg(ypr[2]));
        }
        // ESP_LOGI("IMUC","pci: %f, rci: %f\n", pcval_imu, rcval_imu);
        prev_ypr[0] = ypr[0];
        prev_ypr[1] = ypr[1];
        prev_ypr[2] = ypr[2];
        // xQueueGiveMutexRecursive(imu_control_mutex);
    }
    ESP_LOGW(TAG, "END");
    vTaskDelete(NULL);
}


void IRAM_ATTR getFK(){
    TickType_t start = xTaskGetTickCount();
    // static BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    // portENTER_CRITICAL(&terrainMux);
    //optimized
    int16_t legfx = L1 * sin(deg2rad(getAngle(1))), legfy = L1 * cos(deg2rad(getAngle(1)));
    legs[0].x = 0.5f * (dBase +legfx);
    legs[0].y = -0.866025f * (dBase + legfx);
    legs[0].z = L2 + legfy + (distance[0] - dToF);

    legfx = L1 * sin(deg2rad(getAngle(2)));
    legfy = L1 * cos(deg2rad(getAngle(2)));
    legs[1].x = 0.5f * (dBase +legfx);
    legs[1].y = 0.866025f * (dBase + legfx);
    legs[1].z = L2 + legfy + (distance[1] - dToF);

    legfx = L1 * sin(deg2rad(getAngle(3)));
    legfy = L1 * cos(deg2rad(getAngle(3)));
    legs[2].x = (dBase +legfx);
    legs[2].y = 0;
    legs[2].z = L2 + legfy + (distance[2] - dToF);

    /*
    for (int i = 0; i < 3; i++)
        {
            float angle = getAngle(i + 1);
            legs[i].x = cos(deg2rad(120 * (i)-60)) * (d + L1 * sin(deg2rad(angle)));
            legs[i].y = sin(deg2rad(120 * (i) - 60)) * (d + L1*sin(deg2rad(angle)));
            legs[i].z = L2 + L1 * cos(deg2rad(angle)) + (distance[i] - dLidar);    
    */
    // portEXIT_CRITICAL(&terrainMux);
    ESP_LOGI("FK"," time: %ld",pdTICKS_TO_MS(xTaskGetTickCount() - start));
    // vTaskNotifyGiveFromISR(TaskHandle_terrain, &xHigherPriorityTaskWoken);
    // xSemaphoreGiveFromISR(legs_fk_sem, &xHigherPriorityTaskWoken);
    // if(xHigherPriorityTaskWoken == pdTRUE){
    //     portYIELD_FROM_ISR();
    // }
}

int16_t getDist2Psqrt(VectorInt16 a, VectorInt16 b){
    return (sq(a.x - b.x) + sq(a.y - b.y) + sq(a.z - b.z));
}

float *getIK(){
    float *angles = (float *)malloc(3 * sizeof(float));
    angles[0] = acos((getDist2Psqrt(VectorInt16(dBase*-0.5f,dBase*0.866025f,0),legs[0]) - 16900) / (12000));
    angles[1] = acos((getDist2Psqrt(VectorInt16(dBase* 0.5f,dBase*0.866025f,0),legs[1]) - 16900) / (12000));
    angles[2] = acos((getDist2Psqrt(VectorInt16(dBase      ,0              ,0),legs[2]) - 16900) / (12000));
    /*
    angles[0] = acos((getDist2P(VectorInt16(dBase*cos(120),dBase*sin(120),0),legs[0]) - sq(L1) - sq(L2)) / (2 * L1 * L2));
    angles[1] = acos((getDist2P(VectorInt16(dBase*cos(60),dBase*sin(60),0),legs[1]) - sq(L1) - sq(L2)) / (2 * L1 * L2));
    angles[2] = acos((getDist2P(VectorInt16(dBase*cos(0),dBase*sin(0),0),legs[2]) - sq(L1) - sq(L2)) / (2 * L1 * L2));
    */
    return angles;
}

void searchAngleFK(){
    target_servo_angle_terrain[0] = -1;
    target_servo_angle_terrain[1] = -1;
    target_servo_angle_terrain[2] = -1;
    TickType_t start = xTaskGetTickCount();
    float angle[3] = {0,0,0};
    angle[0] = getAngle(1);
    angle[1] = getAngle(2);
    angle[2] = getAngle(3);
    VectorInt16 tmp_legs[3];
    VectorFloat n;
    vTaskDelay(5 / portTICK_PERIOD_MS);
    while (pdTICKS_TO_MS(xTaskGetTickCount() - start) < 3000)
    {
        angle[0] = getAngle(1);
        angle[1] = getAngle(2);
        angle[2] = getAngle(3);
        float tmp_pitch = 0.0f, tmp_roll = 0.0f;
        int16_t legfx = L1 * sin(deg2rad(angle[0])), legfy = L1 * cos(deg2rad(angle[0]));
        tmp_legs[0].x = -0.5f * (dBase +legfx);
        tmp_legs[0].y = 0.866025f * (dBase + legfx);
        tmp_legs[0].z = L2 + legfy;
        legfx = L1 * sin(deg2rad(angle[1]));
        legfy = L1 * cos(deg2rad(angle[1]));
        tmp_legs[1].x = 0.5f * (dBase +legfx);
        tmp_legs[1].y = 0.866025f * (dBase + legfx);
        tmp_legs[1].z = L2 + legfy;
        legfx = L1 * sin(deg2rad(angle[2]));
        legfy = L1 * cos(deg2rad(angle[2]));
        tmp_legs[2].x = (dBase +legfx);
        tmp_legs[2].y = 0;
        tmp_legs[2].z = L2 + legfy;
        n = getPlaneNorm(tmp_legs[0], tmp_legs[1], tmp_legs[2]);
        // pitch and roll
        // pitch and roll
        ESP_LOGI("TAG_NORM_SEARCH","%ld, %3.2f, %3.2f, %3.2f",xTaskGetTickCount(), n.x, n.y, n.z);
        
        tmp_pitch = rad2deg(atan2(n.x, n.z));
        tmp_roll = -rad2deg(atan2(n.y, n.z));


        // if(terrain_pitch > 90){
        //     terrain_pitch -= 180;
        // }
        // else if(terrain_pitch < -90){
        //     terrain_pitch += 180;
        // }
        vTaskDelay(5 / portTICK_PERIOD_MS);
        if(abs(tmp_roll-terrain_roll) < 5 && abs(tmp_pitch - terrain_pitch) < 5){
            target_servo_angle_terrain[0] = angle[0];
            target_servo_angle_terrain[1] = angle[1];
            target_servo_angle_terrain[2] = angle[2];
            break;
        }
        angle[0] += (terrain_pitch-tmp_pitch) < 0 ? angle[0] + 0.5 : angle[0] - 0.5;
        angle[1] += (terrain_pitch-tmp_pitch) < 0 ? angle[1] + 0.5 : angle[1] - 0.5;
        angle[2] -= (terrain_pitch-tmp_pitch) < 0 ? angle[2] + 0.5 : angle[2] - 0.5;

        angle[0] -= (terrain_roll-tmp_roll) > 0 ? angle[0] + 0.5 : angle[0] - 0.5;
        angle[1] += (terrain_roll-tmp_roll) > 0 ? angle[1] + 0.5 : angle[1] - 0.5;
        // angle[2] = (terrain_roll-tmp_roll) > 0 ? angle[2] + 0.5 : angle[2] - 0.5;

        angle[0] = angle[0] > 90 ? 90 : angle[0] < 0 ? 0 : angle[0];
        angle[1] = angle[1] > 90 ? 90 : angle[1] < 0 ? 0 : angle[1];
        angle[2] = angle[2] > 90 ? 90 : angle[2] < 0 ? 0 : angle[2];
        ESP_LOGD("TAG_SEARCH","time %ld,  %3.2f, %3.2f, %3.2f", (xTaskGetTickCount()-start), angle[0], angle[1], angle[2]);
    }
    if(target_servo_angle_terrain[0] == -1|| target_servo_angle_terrain[1] == -1|| target_servo_angle_terrain[2] == -1){
        ESP_LOGW("TAG", "No solution found");
    }
    else{
        ESP_LOGD("TAG", "Solution found");
        printf("Solution : %d, %d, %d\n", target_servo_angle_terrain[0], target_servo_angle_terrain[1], target_servo_angle_terrain[2]);
    }
}

void getTerrainTask(void *arg){
    // float pitch = 0, roll = 0;
   
    TickType_t lastTime = xTaskGetTickCount();
    for (;;){
        vTaskDelayUntil(&lastTime, 100 / portTICK_PERIOD_MS);
        while (abs(ypr[1]) > 1 && abs(ypr[2]) > 1)
        {
            ESP_LOGW("TAG", "Drone not stable");
            vTaskDelay(5 / portTICK_PERIOD_MS);
        }
        vTaskDelay(1/portTICK_PERIOD_MS);
        if(distance[0] == 0 && distance[1] == 0 && distance[2] == 0){
            ESP_LOGW("TAG", "drone already landed or no terrain detected");
        }
        // printf("\n");

        VectorFloat n = getPlaneNorm(legs[0], legs[1], legs[2]);

        ESP_LOGI("TAG_NORM_TERRAIN","%ld, %3.2f, %3.2f, %3.2f",xTaskGetTickCount(), n.x, n.y, n.z);

        // pitch and roll
        
        terrain_pitch = -rad2deg(atan2(n.x, n.z))+180;
        terrain_roll = -rad2deg(atan2(n.y, n.z));
        
        // if(terrain_pitch > 90){
        //     terrain_pitch -= 180;
        // }
        // else if(terrain_pitch < -90){
        //     terrain_pitch += 180;
        // }

        // terrain_pitch = rad2deg(atan2(n.y, n.z)-PI);
        // terrain_roll = -rad2deg(atan2(n.x, n.z));   
        vTaskDelay(1/portTICK_PERIOD_MS);
        // printf("point: ");
        getFK();
        searchAngleFK();

        } 
}

void lidarControlTask(void *pvParameter){
    TickType_t lastTime;
    int z_last[3]={0,0,0};
    lastTime = xTaskGetTickCount();
    for (;;){
        vTaskDelayUntil(&lastTime, 200 / portTICK_PERIOD_MS);
        // printf("point: ");
        for (int i = 0; i < 3; i++)
        {
            float angle = getAngle(i + 1);
            legs[i].x = cos(deg2rad(120 * (i)-60)) * (dBase + L1 * sin(deg2rad(angle)));
            legs[i].y = sin(deg2rad(120 * (i) - 60)) * (dBase + L1*sin(deg2rad(angle)));
            if(distance[i] == 0){
                legs[i].z = z_last[i];
            }
            else{
                legs[i].z = L2 + L1 * cos(deg2rad(angle)) + (distance[i] - dToF);
                z_last[i] = legs[i].z;
            }
            // printf("%d-(%d, %d, %d)\t", i, legs[i].x, legs[i].y, legs[i].z);
        }
        if(distance[0] == 0 && distance[1] == 0 && distance[2] == 0){
            pcval_lidar = 0;
            rcval_lidar = 0;
            continue;
        }
        // printf("\n");

        VectorFloat n = getPlaneNorm(legs[0], legs[1], legs[2]);

        // pitch and roll
        // pitch and roll
        terrain_pitch = rad2deg(atan2(n.x,n.z))+180;
        terrain_roll = -rad2deg(atan2(n.y, n.z));

        pcval_lidar = pid_pitch->update(0, terrain_pitch);
        rcval_lidar = pid_roll->update(0,  terrain_roll);
    }
}


void servoSweepTask(void *pvParameter){
    // printf("Servo Sweep Task\n");
    // xSemaphoreTakeRecursive(servo_sem, 10/portTICK_PERIOD_MS);
    for (int i = 0;i<90;i++){
        servo->writeMicroseconds(0,PWM1(i));
        servo->writeMicroseconds(1,PWM2(i));
        servo->writeMicroseconds(2,PWM3(i));
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    // xSemaphoreGiveRecursive(servo_sem);
    vTaskDelete(NULL);
}

// MARK: Initialize
void initServoTask(void *pvParameter)
{
    ESP_LOGI(TAG, "Initializing Servos");
    xSemaphoreTake(servo_sem, 10/portTICK_PERIOD_MS);
    // SemaphoreHandle_t init_mutex = xSemaphoreCreateBinary();
    
    // servo->begin(&init_mutex);
    servo->begin();
    // xSemaphoreTakeRecursive(init_mutex, 10/portTICK_PERIOD_MS);
    xSemaphoreGive(servo_sem);

    vTaskDelete(NULL);
}


// MARK: Button
const char *button_event_table[] = 
{
    "BUTTON_PRESS_DOWN",
    "BUTTON_PRESS_UP",
    "BUTTON_PRESS_REPEAT",
    "BUTTON_PRESS_REPEAT_DONE",
    "BUTTON_SINGLE_CLICK",
    "BUTTON_DOUBLE_CLICK",
    "BUTTON_MULTIPLE_CLICK",
    "BUTTON_LONG_PRESS_START",
    "BUTTON_LONG_PRESS_HOLD",
    "BUTTON_LONG_PRESS_UP",
};


static void button_event_single_cb(void *arg, void *data)
{

    // printf("single click on button %d\n", (int)data);
    ESP_LOGD(TAG_BUTTON, "single click on button %d", (int)data);
    int pin = (int)data;
    if(pin == 18){
        // resume and suspend terrain task, create if not exist
        if(TaskHandle_terrain == NULL){
            xTaskCreatePinnedToCore(getTerrainTask, "terrain task", 2048*2, NULL, 2, &TaskHandle_terrain, 0);
        }
        else{
            eTaskState state = eTaskGetState(TaskHandle_terrain);
            if(state == eSuspended){
                vTaskResume(TaskHandle_terrain);
                ESP_LOGD(TAG, "Terrain Task resumed");
            }
            else if(state != eSuspended){
                vTaskSuspend(TaskHandle_terrain);
                ESP_LOGD(TAG, "Terrain Task suspended");
            }
        } 
    }
    else if(pin == 5){
        
        eTaskState state = eTaskGetState(TaskHandle_suspended);
        if(state == eSuspended){
            vTaskResume(TaskHandle_suspended);
            vTaskDelay(5);
            vTaskSuspend(TaskHandle_servoWrite);
            landing_gear_state = SUSPENDED;
            ESP_LOGD(TAG, "Suspended Task resumed");
        }
        else if(state != eSuspended){
            landing_gear_state = DESCENDING;
            vTaskSuspend(TaskHandle_suspended);
            vTaskDelay(5);
            vTaskResume(TaskHandle_servoWrite);
            ESP_LOGD(TAG, "Suspended Task suspended");
        }
    }
}

static void button_event_long_cb(void *arg, void *data)
{

    // printf("long press on button %d\n", (int)data);
    // int pin = (int)data;
    
}

void button_event_init(button_handle_t btn, int gpio_num)
{
    button_event_config_t btn_event_cfg {
        .event = BUTTON_LONG_PRESS_START,
        .event_data{ .long_press {
            .press_time = 2000
            }
        }
    };
    iot_button_register_event_cb(btn, btn_event_cfg, button_event_long_cb, (void *)gpio_num);
}

button_handle_t button_init(uint32_t button_num)
{
    button_config_t btn_cfg = {
        .type = BUTTON_TYPE_GPIO,
        .gpio_button_config = {
            .gpio_num = (int32_t)button_num,
            .active_level = 1,
    #if CONFIG_GPIO_BUTTON_SUPPORT_POWER_SAVE
            .enable_power_save = true,
    #endif
        },
    };
    button_handle_t btn = iot_button_create(&btn_cfg);
    assert(btn);
    esp_err_t err = iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, button_event_single_cb, (void *)button_num);
    // esp_err_t err = iot_button_register_cb(btn, BUTTON_PRESS_DOWN, button_event_single_cb, (void *)button_num);
    // err |= iot_button_register_cb(btn, BUTTON_PRESS_UP, button_event_single_cb, (void *)button_num);
    // err |= iot_button_register_cb(btn, BUTTON_PRESS_REPEAT, button_event_single_cb, (void *)button_num);
    // err |= iot_button_register_cb(btn, BUTTON_PRESS_REPEAT_DONE, button_event_single_cb, (void *)button_num);
    // err |= iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, button_event_single_cb, (void *)button_num);
    // err |= iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, button_event_single_cb, (void *)button_num);
    // err |= iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, button_event_single_cb, (void *)button_num);
    // err |= iot_button_register_cb(btn, BUTTON_LONG_PRESS_HOLD, button_event_single_cb, (void *)button_num);
    // err |= iot_button_register_cb(btn, BUTTON_LONG_PRESS_UP, button_event_single_cb, (void *)button_num);
    ESP_ERROR_CHECK(err);
    return btn;
}

void buttonInitTask(void *pvParameter){
    button_event_init(button_init(18),18);
    button_event_init(button_init(5),5);
    vTaskDelete(NULL);
}

void suspendedTask(void *pvParameter){
    for (;;)
    {
        servo->writeMicroseconds(0, PWM1(60));
        servo->writeMicroseconds(1, PWM2(60));
        servo->writeMicroseconds(2, PWM3(60));
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void sendLogTask(void *pvParameter){
    #ifdef CONFIG_LOG_DATA_TO_SERIAL
        #ifdef CONFIG_LOG_SERVO
            ESP_LOGI("LOG_SERVO", "T,pwm1,pwm2,pwm3");
        #endif
        #ifdef CONFIG_LOG_PID
            ESP_LOGI("LOG_PID", "T,pcl,rcl,pci,rci");
        #endif
        #ifdef CONFIG_LOG_IMU
            ESP_LOGI("LOG_IMU", "T,yaw,pitch,roll");
        #endif    
        #ifdef CONFIG_LOG_LIDAR
            ESP_LOGI("LOG_LIDAR","T,L1,L2,L3");
        #endif
        #ifdef CONFIG_LOG_ADC
            ESP_LOGI("LOG_ADC","T,a1,a2,a3");
        #endif  
        #ifdef CONFIG_LOG_FK
            ESP_LOGI("LOG_FK", "T,x1,y1,z1,x2,y2,z2,x3,y3,z3,pitch,roll");
        #endif
        #ifdef CONFIG_LOG_LIDAR
            ESP_LOGI("LOG_LIDAR","T,L1,L2,L3");
        #endif
        #ifdef CONFIG_LOG_ADC
            ESP_LOGI("LOG_ADC","T,a1,a2,a3");
        #endif  
        #ifdef CONFIG_LOG_FK
            ESP_LOGI("LOG_FK", "T,x1,y1,z1,x2,y2,z2,x3,y3,z3,pitch,roll");
        #endif
    #endif
    TickType_t lastTime = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastTime, 100 / portTICK_PERIOD_MS);    
        #ifdef CONFIG_LOG_DATA_TO_SERIAL
            #ifdef CONFIG_LOG_FK
                ESP_LOGI("LOG_FK", "%d, %d, %d,\t%d, %d, %d,\t%d, %d, %d,\t%3.2f, %3.2f",  legs[0].x, legs[0].y, legs[0].z, legs[1].x, legs[1].y, legs[1].z, legs[2].x, legs[2].y, legs[2].z, terrain_pitch, terrain_roll);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            #endif
            #ifdef CONFIG_LOG_LIDAR
                ESP_LOGI("LOG_LIDAR","%d, %d, %d", distance[0],distance[1],distance[2]);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            #endif
            #ifdef CONFIG_LOG_ADC
                ESP_LOGI("LOG_ADC","%d, %d, %d", adc_value_filtered[0],adc_value_filtered[1],adc_value_filtered[2]);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            #endif
            #ifdef CONFIG_LOG_IMU
                ESP_LOGI("LOG_IMU", "%3.2f, %3.2f, %3.2f",  ypr[0], ypr[1], ypr[2]);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            #endif
            #ifdef CONFIG_LOG_SERVO    
                ESP_LOGI("LOG_SERVO", "%d,%d,%d", pwms[0], pwms[1], pwms[2]);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            #endif
            #ifdef CONFIG_LOG_PID
                ESP_LOGI("LOG_PID", "%3.2f,%3.2f,%3.2f,%3.2f", pcval_lidar, rcval_lidar, pcval_imu, rcval_imu);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            #endif
        #endif
        
    }
}

void initializeTask(void *pvParameter)
{
    // portMUX_TYPE myMutex = portMUX_INITIALIZER_UNLOCKED;
    // taskENTER_CRITICAL(&myMutex);
    

    ESP_LOGD(TAG, "Initializing Tasks");
    xTaskCreatePinnedToCore(buttonInitTask, "button init", 2048*2, NULL, 2, NULL, 1);
    
    ESP_LOGD(TAG, "Initializing Servos");
    xTaskCreatePinnedToCore(initServoTask, "servo task", 6144, NULL, 2, &TaskHandle_initServo, 1);
    ESP_LOGD(TAG, "Initializing LiDAR");
    xTaskCreatePinnedToCore(lidarTask, "LiDAR Task", 4096 , NULL, 2, &TaskHandle_lidar, 1);
    ESP_LOGD(TAG, "Initializing ADC");
    xTaskCreatePinnedToCore(startAdcTask, "adc task", 2048*3, &TaskHandle_ADC, 2, NULL, 1);
    ESP_LOGD(TAG, "Initializing SUSPEND TASK");
    xTaskCreatePinnedToCore(suspendedTask, "suspended task", 2048, NULL, 2, &TaskHandle_suspended, 1);
    vTaskSuspend(TaskHandle_suspended);
    
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP_LOGD(TAG, "Initializing IMU");
    xTaskCreatePinnedToCore(imuTask, "IMU Task", 3072, NULL, 2, &TaskHandle_IMU, 1);
    ESP_LOGD(TAG, "Starting Control Tasks");
    xTaskCreatePinnedToCore(imuControlLoopTask, "imu control", 4096, NULL, 1, &TaskHandle_imuControl, 0);
    
    // xTaskCreatePinnedToCore(lidarControlTask, "lidar control", 4096, NULL, 5, &TaskHandle_lidarControl, 1);
    vTaskDelay(1000/portTICK_PERIOD_MS);
    // xTaskCreatePinnedToCore(servoSweepTask, "ServoSweep", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(servoWriteTask, "servo write", 3072, NULL, 2, &TaskHandle_servoWrite, 1);    
    xTaskCreatePinnedToCore(sendLogTask, "send log", 2048, NULL, 1, NULL, 0);
    
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    // taskEXIT_CRITICAL(&myMutex);     
    landing_gear_state = DESCENDING;
    vTaskDelete(NULL);
}
