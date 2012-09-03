(**
 * libgsl.sml
 *
 * @copyright (c) 2006-2007, Tohoku University.
 * @author UENO Katsuhiro
 * @version $Id: libgsl.sml,v 1.1.2.1 2007/03/26 06:26:50 katsu Exp $
 *)

structure GSL =
struct

  local
    open CConfig
    open DynamicLink

    val libgslName = valOf (findLibrary ("gsl","gsl_error",[]))
(*
    (* Mac OS X *)
    val libgslName = "/sw/lib/libgsl.0.dylib"
*)
    val libgsl = dlopen libgslName

  in

  val fft_real_radix2_transform =
      dlsym (libgsl, "gsl_fft_real_radix2_transform")
      : _import (real array, int, int) -> int

  end

end