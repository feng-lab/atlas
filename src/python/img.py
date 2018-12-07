import numpy as np
from dataclasses import dataclass, field
from typing import List
from . import _imgpy as _C

def _array_eq(arr1, arr2):
    return (isinstance(arr1, np.ndarray) and
            isinstance(arr2, np.ndarray) and
            arr1.shape == arr2.shape and
            (arr1 == arr2).all())


@dataclass(eq=False)
class Img:
    """Class for Image."""
    # ntimes, nchannels, depth, height, width
    datas: List[np.ndarray] = field(default_factory=list)

    valid_bit_count: int = 0
    # z, y, x
    voxel_size_in_um: np.ndarray[np.float64] = np.ndarray([0., 0., 0.])

    time_stamps: List[float] = field(default_factory=list)
    channel_names: List[str] = field(default_factory=list)
    channel_colors: List[tuple] = field(default_factory=list)
    position: List[float] = field(default_factory=list)
    last_channel_is_alpha_channel: bool = False

    def __eq__(self, other):
        if not isinstance(other, Img):
            return NotImplemented
        return self.datas == other.datas and \
               _array_eq(self.voxel_size_in_um, other.voxel_size_in_um)

    width = property(lambda self: self.datas[0].shape[3] if len(self.datas) > 0 else 0)

    height = property(lambda self: self.datas[0].shape[2] if len(self.datas) > 0 else 0)

    depth = property(lambda self: self.datas[0].shape[1] if len(self.datas) > 0 else 0)

    nchannels = property(lambda self: self.datas[0].shape[0] if len(self.datas) > 0 else 0)

    ntimes = property(lambda self: len(self.datas))

    dtype = property(lambda self: self.datas[0].dtype if len(self.datas) > 0 else np.uint8)

    voxel_size_x_in_um = property(lambda self: self.voxel_size_in_um[2],
                                  lambda self, value: self.voxel_size_in_um.itemset(2, value))

    voxel_size_y_in_um = property(lambda self: self.voxel_size_in_um[1],
                                  lambda self, value: self.voxel_size_in_um.itemset(1, value))

    voxel_size_z_in_um = property(lambda self: self.voxel_size_in_um[0],
                                  lambda self, value: self.voxel_size_in_um.itemset(0, value))


# if __name__ == "__main__":

