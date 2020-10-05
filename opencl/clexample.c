/*
 * Copyright © 2020 Google LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <stdio.h>
#include <stdlib.h>

/* Max no of CL implementations, 5 should be enough to find CPU & GPU */
#define MAX_PLATFORMS 5

/* CL Program */
const char *kKernels =
    "__kernel void square(__global int *ARR) {"
    "   ARR[get_global_id(0)] = ARR[get_global_id(0)] * "
    "   ARR[get_global_id(0)];}"
    "__kernel void add_arrays(__global int *ARR1, __global int *ARR2) {"
    "   ARR1[get_global_id(0)] = ARR1[get_global_id(0)] + "
    "   ARR2[get_global_id(0)];}"
    "__kernel void add_const(__global int *ARR, const int c) {"
    "   ARR[get_global_id(0)] = ARR[get_global_id(0)] + c;}";

/* Constant used in the calculations */
const cl_int kAdd = 2;
const size_t kArraySize = 1024;

/*
 * Function to carry out the CL elementwise calculations
 * (out_array = in_array*in_array + in_array + 2)
 */
void Compute(cl_context ctx, cl_command_queue queue,
            int *in_array, int *out_array, size_t size) {
  cl_program program;
  cl_int err;
  cl_mem device_mem_1, device_mem_2;
  cl_kernel kernel_square, kernel_add_const, kernel_add_arrays;

  program = clCreateProgramWithSource(ctx, 1, (const char **) &kKernels,
                                      NULL, &err);
  err = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
  kernel_square = clCreateKernel(program, "square", &err);
  kernel_add_const = clCreateKernel(program, "add_const", &err);
  kernel_add_arrays = clCreateKernel(program, "add_arrays", &err);

  /* Initialize the data buffers */
  device_mem_1 = clCreateBuffer(ctx, CL_MEM_READ_WRITE, size*sizeof(*in_array),
                        NULL, &err);
  device_mem_2 = clCreateBuffer(ctx, CL_MEM_READ_WRITE, size*sizeof(*in_array),
                        NULL, &err);

  /* Write and queue the input buffers */
  err = clEnqueueWriteBuffer(queue, device_mem_1, CL_TRUE, 0, size*sizeof(*in_array),
                              in_array, 0, NULL, NULL);
  err = clEnqueueWriteBuffer(queue, device_mem_2, CL_TRUE, 0, size*sizeof(*in_array),
                              in_array, 0, NULL, NULL);

  /* Calculate the square the elements of the array */
  clSetKernelArg(kernel_square, 0, sizeof(cl_mem), &device_mem_1);
  clEnqueueNDRangeKernel(queue, kernel_square, 1, NULL, &size, &size, 0, NULL, NULL);

  /* Add the current result to the original array */
  clSetKernelArg(kernel_add_arrays, 0, sizeof(cl_mem), &device_mem_1);
  clSetKernelArg(kernel_add_arrays, 1, sizeof(cl_mem), &device_mem_2);
  clEnqueueNDRangeKernel(queue, kernel_add_arrays, 1, NULL, &size, &size, 0, NULL, NULL);

  /* Add a constant to each element */
  clSetKernelArg(kernel_add_const, 0, sizeof(cl_mem), &device_mem_1);
  clSetKernelArg(kernel_add_const, 1, sizeof(int), &kAdd);
  clEnqueueNDRangeKernel(queue, kernel_add_const, 1, NULL, &size, &size, 0, NULL, NULL);


  /* Queue the output buffer, and kick off the calcuations */
  err = clEnqueueReadBuffer(queue, device_mem_1, CL_TRUE, 0, size*sizeof(*out_array),
                            out_array, 0, NULL, NULL);
  err = clFinish(queue);
  clReleaseMemObject(device_mem_1);
  clReleaseMemObject(device_mem_2);
}

/* Helper function to compare results */
int CompareArrays(int *arr1, int *arr2, int no_elements) {
  int success = 1;

  for (int i=0; i<no_elements; i++) {
    if (arr1[i] != arr2[i]) {
      success = 0;
      break;
    }
  }
  return success;
}

int main( void ) {
  cl_int err;
  cl_platform_id platform;
  cl_platform_id platforms[MAX_PLATFORMS];
  cl_device_id device;
  cl_context_properties props[3] = {CL_CONTEXT_PLATFORM, 0, 0};
  cl_context ctx_gpu, ctx_cpu;
  cl_command_queue queue_cpu;
  cl_command_queue queue_gpu;
  cl_event event = NULL;
  int i, num_platforms;

  int *input_arr, *result_cpu_arr, *gpu_result_arr, *expected_result_arr;

  input_arr = (int*) malloc(kArraySize * sizeof(*input_arr));
  gpu_result_arr = (int*) malloc(kArraySize * sizeof(*gpu_result_arr));
  result_cpu_arr = (int*) malloc(kArraySize * sizeof(*result_cpu_arr));
  expected_result_arr = (int*) malloc(kArraySize *
                                      sizeof(*expected_result_arr));

  /* Initialize input and expected arrays*/
  for (i=0; i<kArraySize; i++) {
    input_arr[i] = i;
    expected_result_arr[i] = i*i+i+kAdd;
  }

  /* Get the platforms. */
  err = clGetPlatformIDs(5, platforms, &num_platforms);

  /* Setup one CPU and one GPU device */
  device = NULL;
  ctx_cpu = ctx_gpu = NULL;
  for (i=0; i<num_platforms; i++) {
    props[1] = (cl_context_properties) platforms[i];
    if (ctx_cpu == NULL) {
      err = clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_CPU,
                            1, &device, NULL);
      if (device != NULL) {
        ctx_cpu = clCreateContext(props, 1, &device, NULL, NULL, &err);
        queue_cpu = clCreateCommandQueue(ctx_cpu, device, 0, &err);
        device = NULL;
        continue;
      }
    }
    if (ctx_gpu == NULL) {
      err = clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU,
                            1, &device, NULL);
      if (device != NULL) {
        ctx_gpu = clCreateContext(props, 1, &device, NULL, NULL, &err);
        queue_gpu = clCreateCommandQueue(ctx_gpu, device, 0, &err);
        device = NULL;
        continue;
      }
    }
  }

  /* Run the GPU computation */
  if (ctx_gpu) {
    Compute(ctx_gpu, queue_gpu, input_arr, gpu_result_arr, kArraySize);
    clReleaseCommandQueue(queue_gpu);
    clReleaseContext(ctx_gpu);

    if (CompareArrays(expected_result_arr, gpu_result_arr, kArraySize))
      printf("GPU: Result as expected\n");
    else
      printf("GPU: FAIL Result NOT as expected\n");
  } else {
    printf("GPU: FAIL No OpenCL implementation found\n");
  }

  /* Run the CPU computation */
  if (ctx_cpu) {
    Compute(ctx_cpu, queue_cpu, input_arr, result_cpu_arr, kArraySize);
    clReleaseCommandQueue( queue_cpu );
    clReleaseContext( ctx_cpu );

    if (CompareArrays(expected_result_arr, result_cpu_arr, kArraySize))
      printf("CPU: Result as expected\n");
    else
      printf("CPU: FAIL Result NOT as expected\n");
  } else {
    printf("CPU: FAIL No OpenCL implementation found\n");
  }


  /* Free the allocated memory */
  free(input_arr);
  free(result_cpu_arr);
  free(gpu_result_arr);
  free(expected_result_arr);

  return 0;
}
