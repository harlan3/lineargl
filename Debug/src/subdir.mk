################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../src/lineargl.cpp \
../src/tinyxml2.cpp 

CPP_DEPS += \
./src/lineargl.d \
./src/tinyxml2.d 

OBJS += \
./src/lineargl.o \
./src/tinyxml2.o 


# Each subdirectory must supply rules for building sources it contributes
src/%.o: ../src/%.cpp src/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C++ Compiler'
	g++ -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-src

clean-src:
	-$(RM) ./src/lineargl.d ./src/lineargl.o ./src/tinyxml2.d ./src/tinyxml2.o

.PHONY: clean-src

