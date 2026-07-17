; RUN: opt -mtriple=x86_64-unknown-linux-gnu -vector-library=LIBMVEC -passes=replace-with-veclib -S < %s | FileCheck %s

; Target-specific or unknown intrinsics starting with 'llvm.' that do not have
; a valid Intrinsic::ID (getIntrinsicID() returns Intrinsic::not_intrinsic)
; should be ignored by replace-with-veclib pass instead of crashing.

declare void @llvm.unknown.target.intrinsic.void()
declare <4 x float> @llvm.unknown.target.intrinsic.vec(<4 x float>)

define void @test_unknown_intrinsic_void() {
; CHECK-LABEL: define void @test_unknown_intrinsic_void() {
; CHECK-NEXT:    call void @llvm.unknown.target.intrinsic.void()
; CHECK-NEXT:    ret void
;
  call void @llvm.unknown.target.intrinsic.void()
  ret void
}

define <4 x float> @test_unknown_intrinsic_vec(<4 x float> %x) {
; CHECK-LABEL: define <4 x float> @test_unknown_intrinsic_vec
; CHECK-SAME: (<4 x float> [[X:%.*]]) {
; CHECK-NEXT:    [[R:%.*]] = call <4 x float> @llvm.unknown.target.intrinsic.vec(<4 x float> [[X]])
; CHECK-NEXT:    ret <4 x float> [[R]]
;
  %r = call <4 x float> @llvm.unknown.target.intrinsic.vec(<4 x float> %x)
  ret <4 x float> %r
}
