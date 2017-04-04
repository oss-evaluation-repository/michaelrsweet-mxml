/*
 * Implementation of ZIP container mini-library.
 *
 *     https://github.com/michaelrsweet/zipc
 *
 * Copyright 2017 by Michael R Sweet.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Include necessary headers...
 */

#include "zipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <zlib.h>


/*
 * Local constants...
 */

#define ZIPC_LOCAL_HEADER  0x04034b50	/* Start of a local file header */
#define ZIPC_DIR_HEADER    0x02014b50	/* File header in central directory */
#define ZIPC_DIGITAL_SIG   0x05054b50	/* Digital signature at end */
#define ZIPC_END_RECORD    0x06054b50	/* End of central directory record */

#define ZIPC_MADE_BY       0x0314	/* Version made by UNIX using zip 2.0 */
#define ZIPC_VERSION       0x0014	/* Version needed: 2.0 */

#define ZIPC_FLAG_NONE     0		/* No flags */

#define ZIPC_FLAG_CMASK    0x0006	/* Compression fields */
#define ZIPC_FLAG_CNORMAL  0x0000	/* Normal compression */
#define ZIPC_FLAG_CMAX     0x0002	/* Maximum compression */
#define ZIPC_FLAG_CFAST    0x0004	/* Fast compression */
#define ZIPC_FLAG_CSUPER   0x0006	/* Super fast compression */

#define ZIPC_FLAG_DATA     0x0008	/* Length and CRC-32 fields follow data */

#define ZIPC_COMP_STORE    0		/* No compression */
#define ZIPC_COMP_DEFLATE  8		/* Deflate compression */


/*
 * Local types...
 */

struct _zipc_s
{
  FILE		*fp;			/* ZIP file */
  const char	*error;			/* Last error message */
  int		alloc_files,		/* Allocated file entries in ZIP */
		num_files;		/* Number of file entries in ZIP */
  zipc_file_t	*files;			/* File entries in ZIP */
  unsigned int	modtime;		/* MS-DOS modification date/time */
  z_stream	stream;			/* Deflate stream for current file */
  char		buffer[16384];		/* Deflate output buffer */
};

struct _zipc_file_s
{
  zipc_t	*zc;			/* ZIP container */
  char		filename[256];		/* File/directory name */
  unsigned short flags;			/* General purpose bit flags */
  unsigned short method;		/* Compression method */
  unsigned int	crc32;			/* 32-bit CRC */
  size_t	compressed_size;	/* Size of file (compressed) */
  size_t	uncompressed_size;	/* Size of file (uncompressed) */
  size_t	offset;			/* Offset of this entry in file */
};


/*
 * Local functions...
 */

static zipc_file_t	*zipc_add_file(zipc_t *zc, const char *filename, int compression);
static int		zipc_write(zipc_t *zc, const void *buffer, size_t bytes);
static int		zipc_write_dir_header(zipc_t *zc, zipc_file_t *zf);
static int		zipc_write_local_header(zipc_t *zc, zipc_file_t *zf);
static int		zipc_write_local_trailer(zipc_t *zc, zipc_file_t *zf);
static int		zipc_write_u16(zipc_t *zc, unsigned value);
static int		zipc_write_u32(zipc_t *zc, unsigned value);


/*
 * 'zipcClose()' - Close a ZIP container, writing out the central directory.
 */

int					/* O - 0 on success, -1 on error */
zipcClose(zipc_t *zc)			/* I - ZIP container */
{
  size_t	i;			/* Looping var */
  zipc_file_t	*zf;			/* Current file */
  long		start, end;		/* Central directory offsets */
  int		status = 0;		/* Return status */


 /*
  * First write the central directory headers...
  */

  start = ftell(zc->fp);

  for (i = zc->num_files, zf = zc->files; i > 0; i --, zf ++)
    status |= zipc_write_dir_header(zc, zf);

  end = ftell(zc->fp);

 /*
  * Then write the end of central directory block...
  */

  status |= zipc_write_u32(zc, ZIPC_END_RECORD);
  status |= zipc_write_u16(zc, 0); /* Disk number */
  status |= zipc_write_u16(zc, 0); /* Disk number containing directory */
  status |= zipc_write_u16(zc, zc->num_files);
  status |= zipc_write_u16(zc, zc->num_files);
  status |= zipc_write_u32(zc, end - start);
  status |= zipc_write_u32(zc, (unsigned)start);
  status |= zipc_write_u16(zc, 0); /* file comment length */

  if (fclose(zc->fp))
    status = -1;

  if (zc->alloc_files)
    free(zc->files);

  free(zc);

  return (status);
}


/*
 * 'zipcCreateFile()' - Create a ZIP container file.
 *
 * The "filename" value is the path within the ZIP container.  Directories are
 * separated by the forward slash ("/").
 *
 * The "compressed" value determines whether the file is compressed within the
 * container.
 */

zipc_file_t *				/* I - ZIP container file */
zipcCreateFile(
    zipc_t     *zc,			/* I - ZIP container */
    const char *filename,		/* I - Filename in container */
    int        compressed)		/* I - 0 for uncompressed, 1 for compressed */
{
 /*
  * Add the file and write the header...
  */

  zipc_file_t	*zf = zipc_add_file(zc, filename, compressed);
					/* ZIP container file */


  if (zipc_write_local_header(zc, zf))
    return (NULL);
  else
    return (zf);
}


/*
 * 'zipcCreateFileWithString()' - Add a file whose contents are a string.
 *
 * This function should be used for short ZIP container files like mimetype
 * or container descriptions.
 *
 * Note: Files added this way are not compressed.
 */

int					/* O - 0 on success, -1 on failure */
zipcCreateFileWithString(
    zipc_t     *zc,			/* I - ZIP container */
    const char *filename,		/* I - Filename in container */
    const char *contents)		/* I - Contents of file */
{
  zipc_file_t	*zf = zipc_add_file(zc, filename, 0);
					/* ZIP container file */
  size_t	len = strlen(contents);	/* Length of contents */
  int		status = 0;		/* Return status */


  if (zf)
  {
    zf->uncompressed_size = len;
    zf->compressed_size   = len;
    zf->crc32             = crc32(zf->crc32, (const Bytef *)contents, len);

    status |= zipc_write_local_header(zc, zf);
    status |= zipc_write(zc, contents, len);
  }
  else
    status = -1;

  return (status);
}


/*
 * 'zipcError()' - Return a message describing the last detected error.
 */

const char *				/* O - Error string or NULL */
zipcError(zipc_t *zc)			/* I - ZIP container */
{
  return (zc ? zc->error : NULL);
}


/*
 * 'zipcFileFinish()' - Finish writing to a file in a ZIP container.
 */

int					/* O - 0 on success, -1 on error */
zipcFileFinish(zipc_file_t *zf)		/* I - ZIP container file */
{
  int		status = 0;		/* Return status */
  zipc_t	*zc = zf->zc;		/* ZIP container */


  if (zf->method != ZIPC_COMP_STORE)
  {
    int zstatus;			/* Deflate status */

    while ((zstatus = deflate(&zc->stream, Z_FINISH)) != Z_STREAM_END)
    {
      if (zstatus < Z_OK && zstatus != Z_BUF_ERROR)
      {
        zc->error = "Deflate failed.";
        status = -1;
        break;
      }

      status |= zipc_write(zf->zc, zc->buffer, (char *)zc->stream.next_out - zc->buffer);
      zf->compressed_size += (char *)zc->stream.next_out - zc->buffer;

      zc->stream.next_out  = (Bytef *)zc->buffer;
      zc->stream.avail_out = sizeof(zc->buffer);
    }

    if ((char *)zc->stream.next_out > zc->buffer)
    {
      status |= zipc_write(zf->zc, zc->buffer, (char *)zc->stream.next_out - zc->buffer);
      zf->compressed_size += (char *)zc->stream.next_out - zc->buffer;
    }

    deflateEnd(&zc->stream);
  }

  status |= zipc_write_local_trailer(zc, zf);

  return (status);
}


/*
 * 'zipcFilePrintf()' - Write a formatted string to a file.
 *
 * The "zf" value is the one returned by the @link zipc_start_file@ function
 * used to create the ZIP container file.
 *
 * The "format" value is a standard printf format string and is followed by
 * any arguments needed by the string.
 */

int					/* O - 0 on success, -1 on error */
zipcFilePrintf(zipc_file_t *zf,		/* I - ZIP container file */
	       const char  *format,	/* I - Printf-style format string */
	       ...)			/* I - Additional arguments as needed */
{
  char		buffer[8192];		/* Format buffer */
  va_list	ap;			/* Pointer to additional arguments */


  va_start(ap, format);
  if (vsnprintf(buffer, sizeof(buffer), format, ap) < 0)
  {
    zf->zc->error = strerror(errno);
    va_end(ap);
    return (-1);
  }
  va_end(ap);

  return (zipcFileWrite(zf, buffer, strlen(buffer)));
}


/*
 * 'zipcFilePuts()' - Write a string to a file.
 *
 * The "zf" value is the one returned by the @link zipc_start_file@ function
 * used to create the ZIP container file.
 *
 * The "s" value is literal string that is written to the file.  No newline is
 * added.
 */

int					/* O - 0 on success, -1 on error */
zipcFilePuts(zipc_file_t *zf,		/* I - ZIP container file */
               const char  *s)		/* I - String to write */
{
  return (zipcFileWrite(zf, s, strlen(s)));
}


/*
 * 'zipcFileWrite()' - Write data to a ZIP container file.
 *
 * The "zf" value is the one returned by the @link zipc_file_start@ function
 * used to create the ZIP container file.
 *
 * The "data" value points to the bytes to be written.
 *
 * The "bytes" value specifies the number of bytes to write.
 */

int					/* O - 0 on success, -1 on error */
zipcFileWrite(zipc_file_t *zf,		/* I - ZIP container file */
	      const void  *data,	/* I - Data to write */
	      size_t      bytes)	/* I - Number of bytes to write */
{
  int		status = 0;		/* Return status */
  zipc_t	*zc = zf->zc;		/* ZIP container */


  zf->uncompressed_size += bytes;
  zf->crc32             = crc32(zf->crc32, (const Bytef *)data, bytes);

  if (zf->method == ZIPC_COMP_STORE)
  {
   /*
    * Store the contents as literal data...
    */

    status = zipc_write(zc, data, bytes);
    zf->compressed_size += bytes;
  }
  else
  {
   /*
    * Deflate (compress) the contents...
    */

    int	zstatus;			/* Deflate status */

    zc->stream.next_in  = (Bytef *)data;
    zc->stream.avail_in = (unsigned)bytes;

    while (zc->stream.avail_in > 0)
    {
      if (zc->stream.avail_out < (int)(sizeof(zc->buffer) / 8))
      {
	status |= zipc_write(zf->zc, zc->buffer, (char *)zc->stream.next_out - zc->buffer);
	zf->compressed_size += (char *)zc->stream.next_out - zc->buffer;

	zc->stream.next_out  = (Bytef *)zc->buffer;
	zc->stream.avail_out = sizeof(zc->buffer);
      }

      zstatus = deflate(&zc->stream, Z_NO_FLUSH);

      if (zstatus < Z_OK && zstatus != Z_BUF_ERROR)
      {
        zc->error = "Deflate failed.";
        status = -1;
        break;
      }
    }
  }

  return (status);
}


/*
 * 'zipcFileXMLPrintf()' - Write a formatted XML string to a file.
 *
 * The "zf" value is the one returned by the @link zipc_start_file@ function
 * used to create the ZIP container file.
 *
 * The "format" value is a printf-style format string supporting "%d", "%s",
 * and "%%" and is followed by any arguments needed by the string.  Strings
 * ("%s") are escaped as needed.
 */

int					/* O - 0 on success, -1 on error */
zipcFileXMLPrintf(
    zipc_file_t *zf,			/* I - ZIP container file */
    const char  *format,		/* I - Printf-style format string */
    ...)				/* I - Additional arguments as needed */
{
  int		status = 0;		/* Return status */
  va_list	ap;			/* Pointer to additional arguments */
  char		buffer[65536],		/* Buffer */
		*bufend = buffer + sizeof(buffer) - 6,
					/* End of buffer less "&quot;" */
		*bufptr = buffer;	/* Pointer into buffer */
  const char	*s;			/* String pointer */
  int		d;			/* Number */


  va_start(ap, format);

  while (*format && bufptr < bufend)
  {
    if (*format == '%')
    {
      format ++;

      switch (*format)
      {
        case '%' : /* Substitute a single % */
            format ++;

            *bufptr++ = '%';
            break;

        case 'd' : /* Substitute a single integer */
            format ++;

            d = va_arg(ap, int);
            snprintf(bufptr, bufend - bufptr, "%d", d);
            bufptr += strlen(bufptr);
            break;

        case 's' : /* Substitute a single string */
            format ++;

            s = va_arg(ap, const char *);
            while (*s && bufptr < bufend)
            {
              switch (*s)
              {
                case '&' : /* &amp; */
                    *bufptr++ = '&';
                    *bufptr++ = 'a';
                    *bufptr++ = 'm';
                    *bufptr++ = 'p';
                    *bufptr++ = ';';
                    break;
                case '<' : /* &lt; */
                    *bufptr++ = '&';
                    *bufptr++ = 'l';
                    *bufptr++ = 't';
                    *bufptr++ = ';';
                    break;
                case '>' : /* &gt; */
                    *bufptr++ = '&';
                    *bufptr++ = 'g';
                    *bufptr++ = 't';
                    *bufptr++ = ';';
                    break;
                case '\"' : /* &quot; */
                    *bufptr++ = '&';
                    *bufptr++ = 'q';
                    *bufptr++ = 'u';
                    *bufptr++ = 'o';
                    *bufptr++ = 't';
                    *bufptr++ = ';';
                    break;
                default :
                    *bufptr++ = *s;
                    break;
              }

              s ++;
            }

            if (*s)
            {
	      format += strlen(format);
              status = -1;
              zf->zc->error = "Not enough memory to hold XML string.";
            }
            break;

        default : /* Something else we don't support... */
            format += strlen(format);
            status = -1;
            zf->zc->error = "Unsupported format character - only %%, %d, and %s are supported.";
            break;
      }
    }
    else
      *bufptr++ = *format++;
  }

  va_end(ap);

  if (*format)
  {
    status = -1;
    zf->zc->error = "Not enough memory to hold XML string.";
  }

  if (bufptr > buffer)
    status |= zipcFileWrite(zf, buffer, bufptr - buffer);

  return (status);
}


/*
 * 'zipcOpen()' - Open a ZIP container.
 *
 * Currently the only supported "mode" value is "w" (write).
 */

zipc_t *				/* O - ZIP container */
zipcOpen(const char *filename,		/* I - Filename of container */
         const char *mode)		/* I - Open mode ("w") */
{
  zipc_t	*zc;			/* ZIP container */


 /*
  * Only support write mode for now...
  */

  if (strcmp(mode, "w"))
  {
    errno = EINVAL;
    return (NULL);
  }

 /*
  * Allocate memory...
  */

  if ((zc = calloc(1, sizeof(zipc_t))) != NULL)
  {
    time_t	curtime;		/* Current timestamp */
    struct tm	*curdate;		/* Current date/time */

   /*
    * Open the container file...
    */

    if ((zc->fp = fopen(filename, "wb")) == NULL)
    {
      free(zc);
      return (NULL);
    }

   /*
    * Get the current date/time and convert it to the packed MS-DOS format:
    *
    *     Bits    Description
    *     ------  -----------
    *     0-4     Second
    *     5-10    Minute
    *     11-15   Hour
    *     16-20   Day (1-31)
    *     21-24   Month (1-12)
    *     25-31   Years since 1980
    */

    curtime = time(NULL);
    curdate = gmtime(&curtime);

    zc->modtime = curdate->tm_sec |
                  (curdate->tm_min << 5) |
                  (curdate->tm_hour << 11) |
                  (curdate->tm_mday << 16) |
                  ((curdate->tm_mon + 1) << 21) |
                  ((curdate->tm_year - 80) << 25);
  }

  return (zc);
}


/*
 * 'zipc_add_file()' - Add a file to the ZIP container.
 */

static zipc_file_t *			/* O - New file */
zipc_add_file(zipc_t     *zc,		/* I - ZIP container */
              const char *filename,	/* I - Name of file in container */
              int        compression)	/* I - 0 = uncompressed, 1 = compressed */
{
  zipc_file_t	*temp;			/* File(s) */


  if (zc->num_files >= zc->alloc_files)
  {
   /*
    * Allocate more files...
    */

    zc->alloc_files += 10;

    if (!zc->files)
      temp = malloc(zc->alloc_files * sizeof(zipc_file_t));
    else
      temp = realloc(zc->files, zc->alloc_files * sizeof(zipc_file_t));

    if (!temp)
    {
      zc->error = strerror(errno);
      return (NULL);
    }

    zc->files = temp;
  }

  temp = zc->files + zc->num_files;
  zc->num_files ++;

  memset(temp, 0, sizeof(zipc_file_t));

  strncpy(temp->filename, filename, sizeof(temp->filename) - 1);

  temp->zc     = zc;
  temp->crc32  = crc32(0, NULL, 0);
  temp->offset = ftell(zc->fp);

  if (compression)
  {
    temp->flags  = ZIPC_FLAG_CMAX;
    temp->method = ZIPC_COMP_DEFLATE;

    zc->stream.zalloc = (alloc_func)0;
    zc->stream.zfree  = (free_func)0;
    zc->stream.opaque = (voidpf)0;

    deflateInit2(&zc->stream, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);

    zc->stream.next_out  = (Bytef *)zc->buffer;
    zc->stream.avail_out = sizeof(zc->buffer);
  }

  return (temp);
}


/*
 * 'zipc_write()' - Write bytes to a ZIP container.
 */

static int				/* I - 0 on success, -1 on error */
zipc_write(zipc_t     *zc,		/* I - ZIP container */
           const void *buffer,		/* I - Buffer to write */
           size_t     bytes)		/* I - Number of bytes */
{
  if (fwrite(buffer, bytes, 1, zc->fp))
    return (0);

  zc->error = strerror(ferror(zc->fp));

  return (-1);
}


/*
 * 'zipc_write_dir_header()' - Write a central directory file header.
 */

static int				/* I - 0 on success, -1 on error */
zipc_write_dir_header(
    zipc_t      *zc,			/* I - ZIP container */
    zipc_file_t *zf)			/* I - ZIP container file */
{
  int		status = 0;		/* Return status */
  size_t	filenamelen = strlen(zf->filename);
					/* Length of filename */

  status |= zipc_write_u32(zc, ZIPC_DIR_HEADER);
  status |= zipc_write_u16(zc, ZIPC_MADE_BY);
  status |= zipc_write_u16(zc, ZIPC_VERSION);
  status |= zipc_write_u16(zc, zf->flags);
  status |= zipc_write_u16(zc, zf->method);
  status |= zipc_write_u32(zc, zc->modtime);
  status |= zipc_write_u32(zc, zf->crc32);
  status |= zipc_write_u32(zc, zf->compressed_size);
  status |= zipc_write_u32(zc, zf->uncompressed_size);
  status |= zipc_write_u16(zc, filenamelen);
  status |= zipc_write_u16(zc, 0); /* extra field length */
  status |= zipc_write_u16(zc, 0); /* comment length */
  status |= zipc_write_u16(zc, 0); /* disk number start */
  status |= zipc_write_u16(zc, 0); /* internal file attributes */
  status |= zipc_write_u32(zc, 0); /* external file attributes */
  status |= zipc_write_u32(zc, zf->offset);
  status |= zipc_write(zc, zf->filename, filenamelen);

  return (status);
}


/*
 * 'zipc_write_local_header()' - Write a local file header.
 */

static int				/* I - 0 on success, -1 on error */
zipc_write_local_header(
    zipc_t      *zc,			/* I - ZIP container */
    zipc_file_t *zf)			/* I - ZIP container file */
{
  int		status = 0;		/* Return status */
  unsigned	flags = zf->flags;	/* General purpose flags */
  size_t	filenamelen = strlen(zf->filename);
					/* Length of filename */

  if (zf->uncompressed_size == 0)
    flags |= ZIPC_FLAG_DATA;

  status |= zipc_write_u32(zc, ZIPC_LOCAL_HEADER);
  status |= zipc_write_u16(zc, ZIPC_VERSION);
  status |= zipc_write_u16(zc, flags);
  status |= zipc_write_u16(zc, zf->method);
  status |= zipc_write_u32(zc, zc->modtime);
  status |= zipc_write_u32(zc, zf->uncompressed_size == 0 ? 0 : zf->crc32);
  status |= zipc_write_u32(zc, zf->compressed_size);
  status |= zipc_write_u32(zc, zf->uncompressed_size);
  status |= zipc_write_u16(zc, filenamelen);
  status |= zipc_write_u16(zc, 0); /* extra field length */
  status |= zipc_write(zc, zf->filename, filenamelen);

  return (status);
}


/*
 * 'zipc_write_local_trailer()' - Write a local file trailer.
 */

static int				/* I - 0 on success, -1 on error */
zipc_write_local_trailer(
    zipc_t      *zc,			/* I - ZIP container */
    zipc_file_t *zf)			/* I - ZIP container file */
{
  int	status = 0;			/* Return status */


  status |= zipc_write_u32(zc, zf->crc32);
  status |= zipc_write_u32(zc, zf->compressed_size);
  status |= zipc_write_u32(zc, zf->uncompressed_size);

  return (status);
}


/*
 * 'zipc_write_u16()' - Write a 16-bit unsigned integer.
 */

static int				/* I - 0 on success, -1 on error */
zipc_write_u16(zipc_t   *zc,		/* I - ZIP container */
               unsigned value)		/* I - Value to write */
{
  unsigned char	buffer[2];		/* Buffer */


  buffer[0] = (unsigned char)value;
  buffer[1] = (unsigned char)(value >> 8);

  return (zipc_write(zc, buffer, sizeof(buffer)));
}


/*
 * 'zipc_write_u32()' - Write a 32-bit unsigned integer.
 */

static int				/* I - 0 on success, -1 on error */
zipc_write_u32(zipc_t   *zc,		/* I - ZIP container */
               unsigned value)		/* I - Value to write */
{
  unsigned char	buffer[4];		/* Buffer */


  buffer[0] = (unsigned char)value;
  buffer[1] = (unsigned char)(value >> 8);
  buffer[2] = (unsigned char)(value >> 16);
  buffer[3] = (unsigned char)(value >> 24);

  return (zipc_write(zc, buffer, sizeof(buffer)));
}