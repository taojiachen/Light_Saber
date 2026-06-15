idf_component_register(SRC_DIRS "." "app" "app/wifi" "app/audio" "app/led_strip" "app/key" "app/tcp" "app/imu" "app/websocket" "app/state_machine" "app/angle" "app/aliyun"
                    INCLUDE_DIRS "." "app" "app/wifi" "app/audio" "app/led_strip" "app/key" "app/tcp" "app/imu" "app/websocket" "app/state_machine" "app/angle" "app/aliyun"
                    )
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)