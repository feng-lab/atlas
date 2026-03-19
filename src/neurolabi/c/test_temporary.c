#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utilities.h"
#include "tz_string.h"
#include "tz_swc_tree.h"
#include "tz_workspace.h"
#include "tz_stack_graph.h"
#include "tz_stack_threshold.h"
#include "tz_stack_attribute.h"
#include "tz_stack_utils.h"
#include "tz_int_histogram.h"
#include "tz_graph.h"
#include "tz_math.h"
#include "tz_local_neuroseg.h"
#include "tz_locseg_chain_com.h"
#include "tz_locseg_chain.h"
#include "image_lib.h"
#include "tz_stack_stat.h"
#include "tz_swc_cell.h"
#include "tz_image_io.h"
#include "tz_stack_neighborhood.h"
#include "tz_geo3d_ball.h"
#include "tz_apo.h"


int main(int argc, char *argv[])
{
  const char* home = getenv("HOME");
  char input_path[1024];
  char output_path[1024];
  snprintf(input_path, sizeof(input_path), "%s/%s", home ? home : ".", "enhanced.raw");
  snprintf(output_path, sizeof(output_path), "%s/%s", home ? home : ".", "enhanced1.raw");

  Stack* stack = Read_Raw_Stack_C(input_path, 0);
  size_t start = (size_t)stack->height * stack->width * stack->kind * 85;
  for (; start < (size_t)stack->height * stack->width * stack->kind * 95; start++) {
    if (*(uint8*)(stack->array+start)  != 0)
      printf("%d\n",*(uint8*)(stack->array+start) );
  }
  Write_Stack_U(output_path, stack, NULL);
  return 0;
}
