(* dummy implementation of SMLofNJ structure *)
structure SMLofNJ : sig
  val exnHistory : exn -> string list
end =
struct
  fun exnHistory (_:exn) = nil : string list
end
