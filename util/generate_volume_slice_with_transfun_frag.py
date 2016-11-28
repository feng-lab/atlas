import sys

import common_dirs


HEADER = """
"""

LOOP1 = """#if NUM_VOLUMES >= {*1*}
uniform sampler3D volume_{*1*};
uniform sampler1D transfer_function_{*1*};
#endif

"""

PART1 = """#if GLSL_VERSION >= 130
in vec3 texCoord0;
#else
varying vec3 texCoord0;
#endif

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

void main()
{
#if NUM_VOLUMES > 0
  vec4 color = vec4(0.0);
  vec4 chColor;

"""

LOOP2 = """#if NUM_VOLUMES >= {*1*}
#if GLSL_VERSION >= 130
  chColor = texture(transfer_function_{*1*}, texture(volume_{*1*}, texCoord0).r);
#else
  chColor = texture1D(transfer_function_{*1*}, texture3D(volume_{*1*}, texCoord0).r);
#endif
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

"""

FOOT = """#ifdef RESULT_OPAQUE
  color.a = 1.0;
#else
  if (color.a == 0.0)
    discard;
#endif

  color.rgb *= color.a;
  FragData0 = color;
#else
  discard;
#endif
}

"""


def generate_volume_slice_with_transfun_frag(file: str, max_num_volumes: int):
    """

    """
    with open(file, mode='w', encoding='utf-8') as f:
        f.write(HEADER)

        for i in range(max_num_volumes):
            f.write(LOOP1.replace('{*1*}', "{0}".format(i+1)))

        f.write(PART1)

        for i in range(max_num_volumes):
            f.write(LOOP2.replace('{*1*}', "{0}".format(i+1)))

        f.write(FOOT)


if __name__ == "__main__":
    if len(sys.argv) > 2:
        generate_volume_slice_with_transfun_frag(sys.argv[1], sys.argv[2])
    else:
        generate_volume_slice_with_transfun_frag(common_dirs.atlas_dir() +
                                                 '/Resources/shader/volume_slice_with_transfun.frag',
                                                 20)
