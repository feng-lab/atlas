import numpy as np
from dataclasses import dataclass, field
from typing import List


@dataclass
class Col4:
    r: np.uint8 = 0
    g: np.uint8 = 0
    b: np.uint8 = 0
    a: np.uint8 = 255


def _array_eq(arr1, arr2):
    return (isinstance(arr1, np.ndarray) and
            isinstance(arr2, np.ndarray) and
            arr1.shape == arr2.shape and
            (arr1 == arr2).all())


@dataclass(eq=False)
class VoxelCoordinate:
    # t, c, z, y, x
    data: np.ndarray = np.zeros(5, dtype=np.int32)

    @classmethod
    def end(cls):
        # Do any necessary preparations, use the `string`
        return cls(data=np.ones(5, dtype=np.int32) * -1)

    def __array__(self, dtype=None):
        if dtype and dtype != self.data.dtype:
            return self.data.astype(dtype)
        else:
            return self.data

    def __eq__(self, other):
        if not isinstance(other, VoxelCoordinate):
            return NotImplemented
        return _array_eq(self.data, other.data)

    x = property(lambda self: self.data[0],
                 lambda self, value: self.data.itemset(0, value))

    y = property(lambda self: self.data[1],
                 lambda self, value: self.data.itemset(1, value))

    z = property(lambda self: self.data[2],
                 lambda self, value: self.data.itemset(2, value))

    c = property(lambda self: self.data[3],
                 lambda self, value: self.data.itemset(3, value))

    t = property(lambda self: self.data[4],
                 lambda self, value: self.data.itemset(4, value))


@dataclass(eq=False)
class ImgInfo:
    """Class for Image Info."""
    # ntimes, nchannels, depth, height, width
    size: np.ndarray = np.ndarray([1, 1, 1, 0, 0])

    voxel_format: np.dtype = np.uint8
    valid_bit_count: int = 0
    # z, y, x
    voxel_size_in_um: np.ndarray[np.float64] = np.ndarray([0., 0., 0.])

    time_stamps: List[float] = field(default_factory=list)
    channel_names: List[str] = ['Ch1']
    channel_colors: List[Col4] = [Col4(r=255, g=255, b=255, a=255)]
    position: List[float] = field(default_factory=list)
    last_channel_is_alpha_channel: bool = False

    def __eq__(self, other):
        if not isinstance(other, ImgInfo):
            return NotImplemented
        return _array_eq(self.size, other.size) and \
               self.voxel_format == other.voxel_format and \
               _array_eq(self.voxel_size_in_um, other.voxel_size_in_um)

    width = property(lambda self: self.size[4],
                     lambda self, value: self.size.itemset(4, value))

    height = property(lambda self: self.size[3],
                      lambda self, value: self.size.itemset(3, value))

    depth = property(lambda self: self.size[2],
                     lambda self, value: self.size.itemset(2, value))

    nchannels = property(lambda self: self.size[1],
                         lambda self, value: self.size.itemset(1, value))

    ntimes = property(lambda self: self.size[0],
                      lambda self, value: self.size.itemset(0, value))

    voxel_size_x_in_um = property(lambda self: self.voxel_size_in_um[2],
                                  lambda self, value: self.voxel_size_in_um.itemset(2, value))

    voxel_size_y_in_um = property(lambda self: self.voxel_size_in_um[1],
                                  lambda self, value: self.voxel_size_in_um.itemset(1, value))

    voxel_size_z_in_um = property(lambda self: self.voxel_size_in_um[0],
                                  lambda self, value: self.voxel_size_in_um.itemset(0, value))


if __name__ == "__main__":
    # execute only if run as a script
    vs = VoxelCoordinate()
    ve = VoxelCoordinate.end()
    print(vs.x)
    print(ve.x)
    vs.x = 3
    ve.z = 5
    print(vs, ve)
    vs = VoxelCoordinate(np.add(vs, ve))
    print(vs)
    print(vs == ve)
    vs = np.add(vs, ve)
    print(vs)
    print(ve)
    print(ve == vs)
    print(VoxelCoordinate.__name__)
