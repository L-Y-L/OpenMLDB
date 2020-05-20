package com._4paradigm.fesql.offline.nodes

import com._4paradigm.fesql.offline._
import com._4paradigm.fesql.offline.utils.FesqlUtil
import com._4paradigm.fesql.vm.{CoreAPI, FeSQLJITWrapper, PhysicalSimpleProjectNode, PhysicalTableProjectNode}
import org.apache.spark.broadcast.Broadcast
import org.apache.spark.sql.Row
import org.apache.spark.sql.types.StructType


object RowProjectPlan {

  /**
   * TODO(baoxinqi): PhysicalColumnProjectNodefn_name gen --> df.select(column1, column2, ...)
   * @param ctx
   * @param node
   * @param inputs
   * @return
   */
  def gen(ctx: PlanContext, node: PhysicalSimpleProjectNode, inputs: Seq[SparkInstance]): SparkInstance = {
    val inputInstance = inputs.head
    val inputRDD = inputInstance.getRDD

    val inputSchemaSlices = FesqlUtil.getOutputSchemaSlices(node.GetProducer(0))
    val outputSchemaSlices = FesqlUtil.getOutputSchemaSlices(node)
    val outputSchema = FesqlUtil.getSparkSchema(node.GetOutputSchema())

    // spark closure
    val projectConfig = ProjectConfig(
      functionName = node.project().fn_info().fn_name(),
      moduleTag = ctx.getTag,
      moduleBroadcast = ctx.getModuleBufferBroadcast,
      inputSchemaSlices = inputSchemaSlices,
      outputSchemaSlices = outputSchemaSlices
    )

    // project implementation
    val projectRDD = inputRDD.mapPartitions(iter => {
      // ensure worker native
      val tag = projectConfig.moduleTag
      val buffer = projectConfig.moduleBroadcast.value.getBuffer
      JITManager.initJITModule(tag, buffer)
      val jit = JITManager.getJIT(tag)

      projectIter(iter, jit, projectConfig)
    })

    SparkInstance.fromRDD(outputSchema, projectRDD)
  }

  def gen(ctx: PlanContext, node: PhysicalTableProjectNode, inputs: Seq[SparkInstance]): SparkInstance = {
    val inputInstance = inputs.head
    val inputRDD = inputInstance.getRDD

    val inputSchemaSlices = FesqlUtil.getOutputSchemaSlices(node.GetProducer(0))
    val outputSchemaSlices = FesqlUtil.getOutputSchemaSlices(node)
    val outputSchema = FesqlUtil.getSparkSchema(node.GetOutputSchema())

    // spark closure
    val projectConfig = ProjectConfig(
      functionName = node.project().fn_name(),
      moduleTag = ctx.getTag,
      moduleBroadcast = ctx.getModuleBufferBroadcast,
      inputSchemaSlices = inputSchemaSlices,
      outputSchemaSlices = outputSchemaSlices
    )

    // project implementation
    val projectRDD = inputRDD.mapPartitions(iter => {
      // ensure worker native
      val tag = projectConfig.moduleTag
      val buffer = projectConfig.moduleBroadcast.value.getBuffer
      JITManager.initJITModule(tag, buffer)
      val jit = JITManager.getJIT(tag)

      projectIter(iter, jit, projectConfig)
    })

    SparkInstance.fromRDD(outputSchema, projectRDD)
  }


  def projectIter(inputIter: Iterator[Row], jit: FeSQLJITWrapper, config: ProjectConfig): Iterator[Row] = {
    // reusable output row inst
    val outputFields = config.outputSchemaSlices.map(_.size).sum
    val outputArr = Array.fill[Any](outputFields)(null)

    val fn = jit.FindFunction(config.functionName)

    // TODO: these objects are now leaked
    val bufferPool = new NativeBufferPool
    val encoder = new SparkRowCodec(config.inputSchemaSlices, bufferPool)
    val decoder = new SparkRowCodec(config.outputSchemaSlices, null)

    inputIter.map(row => projectRow(row, fn, encoder, decoder, outputArr))
  }


  def projectRow(row: Row, fn: Long,
                 encoder: SparkRowCodec,
                 decoder: SparkRowCodec,
                 outputArr: Array[Any]): Row = {
    // call encode
    val nativeInputRow = encoder.encode(row, keepBuffer=false)

    // call native compute
    val outputNativeRow = CoreAPI.RowProject(fn, nativeInputRow, false)

    // call decode
    decoder.decode(outputNativeRow, outputArr)

    // release swig jni objects
    nativeInputRow.delete()
    outputNativeRow.delete()

    Row.fromSeq(outputArr)  // can reuse backed array
  }


  // spark closure class
  case class ProjectConfig(functionName: String,
                           moduleTag: String,
                           moduleBroadcast: Broadcast[SerializableByteBuffer],
                           inputSchemaSlices: Array[StructType],
                           outputSchemaSlices: Array[StructType])

}
