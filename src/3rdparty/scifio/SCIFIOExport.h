
#ifndef SCIFIO_EXPORT_H
#define SCIFIO_EXPORT_H

#ifdef ITK_STATIC
#  define SCIFIO_EXPORT
#  define SCIFIO_HIDDEN
#else
#  ifndef SCIFIO_EXPORT
#    ifdef SCIFIO_EXPORTS
        /* We are building this library */
#      define SCIFIO_EXPORT 
#    else
        /* We are using this library */
#      define SCIFIO_EXPORT 
#    endif
#  endif

#  ifndef SCIFIO_HIDDEN
#    define SCIFIO_HIDDEN 
#  endif
#endif

#ifndef SCIFIO_DEPRECATED
#  define SCIFIO_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef SCIFIO_DEPRECATED_EXPORT
#  define SCIFIO_DEPRECATED_EXPORT SCIFIO_EXPORT SCIFIO_DEPRECATED
#endif

#ifndef SCIFIO_DEPRECATED_NO_EXPORT
#  define SCIFIO_DEPRECATED_NO_EXPORT SCIFIO_HIDDEN SCIFIO_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef SCIFIO_NO_DEPRECATED
#    define SCIFIO_NO_DEPRECATED
#  endif
#endif

#endif /* SCIFIO_EXPORT_H */
