/*
 * main.cpp
 *
 *  Created on: Aug 2, 2016
 *      Author: Harald Heckmann
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include "sds_lib.h"
#include "neural_net.h"

// class copied from SDSoC matrix multiplication example
class perf_counter
{
public:
     uint64_t tot, cnt, calls;
     perf_counter() : tot(0), cnt(0), calls(0) {};
     inline void reset() { tot = cnt = calls = 0; }
     inline void start() { cnt = sds_clock_counter(); calls++; };
     inline void stop() { tot += (sds_clock_counter() - cnt); };
     inline uint64_t avg_cpu_cycles() { return ((tot+(calls>>1)) / calls); };
};

// activation function
static inline float rectifier(float value) {
	return ((value > 0) ? value : (float) 0.0);
}

// this function calculates the results the way the hardware function does it,
// but on the sequential processor instead of the FPGA. It is used to compare
// the results and runtime with the hardware implementation
void neural_net_golden(float *weights0to1, float *weights1to2,\
		float *input_values, float *output_values,\
		uint16_t c_input, uint16_t c_hidden, uint16_t c_output)
{
	// store propagated values for layer1 (hidden) to calculate the result afterwards
	// malloc, so we don't overflow the stack
	float *result_buffer_l1 = (float *) malloc(sizeof(float) * c_hidden);
	float *_input_values = (float *) malloc(sizeof(float) * c_input);

	uint32_t i,j;
	float result; // caution: numeric overflow

	// l0 to l1: Use activationfunction on output

	for (i=0; i < c_input; i++) {
		_input_values[i] = rectifier(input_values[i]);
	}

	// l0 to l1: propagate and activate for l2
	// scalar product
	for (i=0; i < c_hidden; i++) {
		result = 0;

		for (j=0; j < c_input; j++) {
			result += _input_values[j] * weights0to1[c_input*i+j];
		}

		result_buffer_l1[i] = rectifier(result);
	}

	free(_input_values);

	// l1 over l2 to output: propagate and activate for output
	// scalar product
	for (i=0; i < c_output; i++) {
		result = 0;

		for (j=0; j < c_hidden; j++) {
			result += result_buffer_l1[j] * weights1to2[c_hidden*i+j];
		}

    // apply rectifier on output values
		output_values[i] = rectifier(result);
	}

	free(result_buffer_l1);
	return;
}

// function used to initialize memory with a bitlength cap
void random_init(float *var, fixed *var_hw, uint32_t len, uint32_t cap) {
	uint32_t i;

	for (i=0; i<len; i++) {
		//var[i] = (float) (rand() % ((1 << 15)-2));
		var[i] = ((float) (rand() % cap) / (float) (rand() % cap));

		if ((uint64_t) (rand()%2) == 0) {
			var[i] *= (float) -1.0;
		}

		var_hw[i] = fixed(var[i]);
	}
}


int main(int argc, char* argv[])
{
	// datatransfer size < 300 bytes -> malloc, so we use a fifo
	// >= 300 < 8MB -> sds_alloc, so we use an AXIDMA_SIMPLE || AXIDMA_2D
	// >= 8 MB -> malloc, so we use an AXIDMA_SG (SG = Scatter Gather)
	// SG can handle virtual pages, but demands more resources
	// caution: It is irrelevant whether we use malloc or sds_alloc if the
	// data within will be partially transfered, using the pragma
	// SDS data copy (arg[offset:length]) - so even though sds_alloc is used
	// here, SDSoC will instantiate an AXIDMA_SG data mover, so let's just malloc.
	/*int16_t weights0to1[4] = {1,-1,-1,1};
	int16_t weights1to2[2] = {1,1};
	int16_t inputvalues[2] = {0,0};
	int16_t outputvalue;*/

  // declare xor define the required variables and memory
	uint16_t c_input = 2;
	uint16_t c_hidden = 2;
	uint16_t c_output = 1;
	fixed *weights0to1_hw = (fixed *) malloc(sizeof(fixed) * MAX_INPUT_NODES * MAX_HIDDEN_NODES);
	fixed *weights1to2_hw = (fixed *) malloc(sizeof(fixed) * MAX_HIDDEN_NODES * MAX_OUTPUT_NODES);
	fixed *inputvalues_hw = (fixed *) malloc(sizeof(fixed) * MAX_INPUT_NODES);
	fixed *outputvalue_hw = (fixed *) malloc(sizeof(fixed) * MAX_OUTPUT_NODES);
	float *weights0to1 = (float *) malloc(sizeof(float) * MAX_INPUT_NODES * MAX_HIDDEN_NODES);
	float *weights1to2 = (float *) malloc(sizeof(float) * MAX_HIDDEN_NODES * MAX_OUTPUT_NODES);
	float *inputvalues = (float *) malloc(sizeof(float) * MAX_INPUT_NODES);
	float *outputvalue = (float *) malloc(sizeof(float) * MAX_OUTPUT_NODES);
	perf_counter sw_cnt, hw_cnt;
	unsigned long long sw_cycles, hw_cycles;

  // if malloc was not successful, exit the application
	if (!weights0to1 || !weights1to2 || !inputvalues || !outputvalue) {
		if (weights0to1) free(weights0to1);
		if (weights1to2) free(weights1to2);
		if (inputvalues) free(inputvalues);
		if (outputvalue) free(outputvalue);
		printf("Could not allocate memory\n");
		return 2;
	}

  // if malloc was not successful, exit the application
	if (!weights0to1_hw || !weights1to2_hw || !inputvalues_hw || !outputvalue_hw) {
		if (weights0to1_hw) free(weights0to1_hw);
		if (weights1to2_hw) free(weights1to2_hw);
		if (inputvalues_hw) free(inputvalues_hw);
		if (outputvalue_hw) free(outputvalue_hw);
		printf("Could not allocate memory\n");
		return 2;
	}

  // initialize parameters for the neural network
	weights0to1[0] = 1.0;
	weights0to1[1] = -1.0;
	weights0to1[2] = -1.0;
	weights0to1[3] = 1.0;
	weights1to2[0] = 1.0;
	weights1to2[1] = 1.0;
	inputvalues[0] = 0.0;
	inputvalues[1] = 0.0;
	weights0to1_hw[0] = fixed(weights0to1[0]);
	weights0to1_hw[1] = fixed(weights0to1[1]);
	weights0to1_hw[2] = fixed(weights0to1[2]);
	weights0to1_hw[3] = fixed(weights0to1[3]);
	weights1to2_hw[0] = fixed(weights1to2[0]);
	weights1to2_hw[1] = fixed(weights1to2[1]);
	inputvalues_hw[0] = fixed(inputvalues[0]);
	inputvalues_hw[1] = fixed(inputvalues[1]);

	printf("Changed version 2.0f\n");

  // Executing the KNN for the XOR neural network, measuring the time
	printf("Testing neural_net_golden for XOR with values (0,0)\n");
	sw_cnt.start();
	neural_net_golden(weights0to1, weights1to2,\
				inputvalues, outputvalue, c_input, c_hidden, c_output);
	sw_cnt.stop();
	printf("\tResult: %f\n", *outputvalue);

	inputvalues[0] = 1.0;
	inputvalues[1] = 0.0;

	printf("Testing neural_net_golden for XOR with values (1,0)\n");
	//sw_cnt.start();
	neural_net_golden(weights0to1, weights1to2,\
				inputvalues, outputvalue, c_input, c_hidden, c_output);
	//sw_cnt.stop();
	printf("\tResult: %f\n", *outputvalue);

	inputvalues[0] = 0.0;
	inputvalues[1] = 1.0;

	printf("Testing neural_net_golden for XOR with values (0,1)\n");
	//sw_cnt.start();
	neural_net_golden(weights0to1, weights1to2,\
				inputvalues, outputvalue, c_input, c_hidden, c_output);
	//sw_cnt.stop();
	printf("\tResult: %f\n", *outputvalue);

	inputvalues[0] = 1.0;
	inputvalues[1] = 1.0;

	printf("Testing neural_net_golden for XOR with values (1,1)\n");
	//sw_cnt.start();
	neural_net_golden(weights0to1, weights1to2,\
				inputvalues, outputvalue, c_input, c_hidden, c_output);
	//sw_cnt.stop();
	printf("\tResult: %f\n", *outputvalue);

	printf("---------------------------\n");
	printf("Initializing neural net...\n");

  // initializing neural network on the FPGA
	neural_net(1,0,c_input,c_hidden,c_output,weights0to1_hw,weights1to2_hw,NULL,NULL);
	printf("Neural Net initialized\n");

  // executing neural network on the FPGA
	printf("Testing values (0,0)\n");
	hw_cnt.start();
	neural_net(0,1,c_input,c_hidden,c_output,NULL,NULL,inputvalues_hw,outputvalue_hw);
	hw_cnt.stop();
	printf("Result: %f\n", outputvalue_hw->to_float());

	printf("Testing values (1,0)\n");
	inputvalues_hw[0] = fixed(1.0);
	inputvalues_hw[1] = fixed(0.0);
	//hw_cnt.start();
	neural_net(0,1,c_input,c_hidden,c_output,NULL,NULL,inputvalues_hw,outputvalue_hw);
	//hw_cnt.stop();
	printf("Result: %f\n", outputvalue_hw->to_float());

	printf("Testing values (0,1)\n");
	inputvalues_hw[0] = fixed(0.0);
	inputvalues_hw[1] = fixed(1.0);
	//hw_cnt.start();
	neural_net(0,1,c_input,c_hidden,c_output,NULL,NULL,inputvalues_hw,outputvalue_hw);
	//hw_cnt.stop();
	printf("Result: %f\n", outputvalue_hw->to_float());

	printf("Testing values (1,1)\n");
	inputvalues_hw[0] = fixed(1.0);
	inputvalues_hw[1] = fixed(1.0);
	//hw_cnt.start();
	neural_net(0,1,c_input,c_hidden,c_output,NULL,NULL,inputvalues_hw,outputvalue_hw);
	//hw_cnt.stop();
	printf("Result: %f\n", outputvalue_hw->to_float());

  // calculating and printing speedup
	sw_cycles = sw_cnt.avg_cpu_cycles();
	hw_cycles = hw_cnt.avg_cpu_cycles();
	double speedup = (double) sw_cycles / (double) hw_cycles;

    printf("Number of CPU cycles for full XOR test of the KNN on PS: %llu\n", sw_cycles);
    printf("Number of CPU cycles for full XOR test of the KNN on PL: %llu\n", hw_cycles);
    printf("Speedup: %f\n", speedup);

    printf("Testing runtime and precision for biggest possible ANN\n");
    // reset cycle counters
	sw_cnt.reset();
	hw_cnt.reset();
	// test biggest arrays

  // seed the random generator
	 struct sysinfo sys_info;
	 int error = sysinfo(&sys_info);
	 if (0 != error) {
		 srand(0);
	 } else {
		 srand((unsigned int) sys_info.uptime);
	 }

  // randomly initialize all weights and the inputvalues
	random_init(weights0to1, weights0to1_hw, MAX_INPUT_NODES*MAX_HIDDEN_NODES, (1 << 15));
	random_init(weights1to2, weights1to2_hw, MAX_HIDDEN_NODES*MAX_OUTPUT_NODES, (1 << 15));
	random_init(inputvalues, inputvalues_hw, MAX_INPUT_NODES, (1 << 15));

	// initialize neural network on hardware
	neural_net(1,0,MAX_INPUT_NODES, MAX_HIDDEN_NODES, MAX_OUTPUT_NODES,\
			weights0to1_hw,weights1to2_hw,NULL,NULL);

	printf("Testing biggest datasets on PS...\n");
	sw_cnt.start();
	neural_net_golden(weights0to1, weights1to2, inputvalues, outputvalue,\
			MAX_INPUT_NODES, MAX_HIDDEN_NODES, MAX_OUTPUT_NODES);
	sw_cnt.stop();
	printf("Testing biggest datasets on PL...\n");
	hw_cnt.start();
	neural_net(0,1,MAX_INPUT_NODES, MAX_HIDDEN_NODES, MAX_OUTPUT_NODES,\
			NULL,NULL,inputvalues_hw,outputvalue_hw);
	hw_cnt.stop();

	char cont = 'n';

  // ask the user if he/she wants to compare the result arrays from the
  // software calculation and the hardware calculation and if yes, print them
	scanf("Compare results? (y/n): %c", &cont);

	if ('n' != cont) {
		uint16_t i;

		printf("Softwarevalue / Hardwarevalue\n");

		for (i=0; i<MAX_OUTPUT_NODES; i++) {
			printf("[%u] %f/%f\n", i, outputvalue[i], outputvalue_hw[i].to_float());
		}
	}

  // calculate and print speedup
	sw_cycles = sw_cnt.avg_cpu_cycles();
	hw_cycles = hw_cnt.avg_cpu_cycles();
	speedup = (double) sw_cycles / (double) hw_cycles;

	printf("Number of CPU cycles for biggest dataset test of the KNN on PS: %llu\n", sw_cycles);
	printf("Number of CPU cycles for biggest dataset test of the KNN on PL: %llu\n", hw_cycles);
  printf("Speedup: %f\n", speedup);

  // free all allocated memory and exit the program
	if (weights0to1) free(weights0to1);
	if (weights1to2) free(weights1to2);
	if (inputvalues) free(inputvalues);
	if (outputvalue) free(outputvalue);
	if (weights0to1_hw) free(weights0to1_hw);
	if (weights1to2_hw) free(weights1to2_hw);
	if (inputvalues_hw) free(inputvalues_hw);
	if (outputvalue_hw) free(outputvalue_hw);
	return 0;
}

// XSIP watermark, do not delete 67d7842dbbe25473c3c32b93c0da8047785f30d78e8a024de1b57352245f9689
