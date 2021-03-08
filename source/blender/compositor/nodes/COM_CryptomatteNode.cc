/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2018, Blender Foundation.
 */

#include "COM_CryptomatteNode.h"
#include "BKE_node.h"
#include "BLI_assert.h"
#include "BLI_hash_mm3.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "COM_ConvertOperation.h"
#include "COM_CryptomatteOperation.h"
#include "COM_MultilayerImageOperation.h"
#include "COM_RenderLayersProg.h"
#include "COM_SetAlphaMultiplyOperation.h"
#include "COM_SetColorOperation.h"
#include <iterator>
#include <string>

CryptomatteNode::CryptomatteNode(bNode *editorNode) : Node(editorNode)
{
  /* pass */
}

void CryptomatteNode::buildInputOperationsFromRenderSource(
    const CompositorContext &context,
    const bNode &node,
    blender::Vector<NodeOperation *> &r_input_operations)
{
  Scene *scene = (Scene *)node.id;
  if (!scene) {
    return;
  }

  BLI_assert(GS(scene->id.name) == ID_SCE);
  Render *render = RE_GetSceneRender(scene);
  RenderResult *render_result = render ? RE_AcquireResultRead(render) : nullptr;

  if (!render_result) {
    return;
  }

  const short cryptomatte_layer_id = 0;
  std::string prefix = ntreeCompositCryptomatteLayerPrefix(&node);
  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    RenderLayer *render_layer = RE_GetRenderLayer(render_result, view_layer->name);
    if (render_layer) {
      LISTBASE_FOREACH (RenderPass *, render_pass, &render_layer->passes) {
        blender::StringRef combined_name =
            blender::StringRef(view_layer->name,
                               strnlen(view_layer->name, sizeof(view_layer->name))) +
            "." +
            blender::StringRef(render_pass->name,
                               strnlen(render_pass->name, sizeof(render_pass->name)));
        if (combined_name.startswith(prefix)) {
          RenderLayersProg *op = new RenderLayersProg(
              render_pass->name, COM_DT_COLOR, render_pass->channels);
          op->setScene(scene);
          op->setLayerId(cryptomatte_layer_id);
          op->setRenderData(context.getRenderData());
          op->setViewName(context.getViewName());
          r_input_operations.append(op);
        }
      }
    }
  }
  RE_ReleaseResult(render);
}

void CryptomatteNode::buildInputOperationsFromImageSource(
    const CompositorContext &context,
    const bNode &node,
    blender::Vector<NodeOperation *> &r_input_operations)
{
  NodeCryptomatte *cryptoMatteSettings = (NodeCryptomatte *)node.storage;
  Image *image = (Image *)node.id;
  BLI_assert(!image || GS(image->id.name) == ID_IM);
  if (!image || image->type != IMA_TYPE_MULTILAYER) {
    return;
  }

  ImageUser *iuser = &cryptoMatteSettings->iuser;
  BKE_image_user_frame_calc(image, iuser, context.getFramenumber());
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, iuser, NULL);

  if (image->rr) {
    int view = 0;
    if (BLI_listbase_count_at_most(&image->rr->views, 2) > 1) {
      if (iuser->view == 0) {
        /* Heuristic to match image name with scene names, check if the view name exists in the
         * image. */
        view = BLI_findstringindex(
            &image->rr->views, context.getViewName(), offsetof(RenderView, name));
        if (view == -1)
          view = 0;
      }
      else {
        view = iuser->view - 1;
      }
    }

    RenderLayer *render_layer = (RenderLayer *)BLI_findlink(&image->rr->layers, iuser->layer);
    if (render_layer) {
      int render_pass_index = 0;
      std::string prefix = ntreeCompositCryptomatteLayerPrefix(&node);
      for (RenderPass *render_pass = (RenderPass *)render_layer->passes.first; render_pass;
           render_pass = render_pass->next, render_pass_index++) {
        if (blender::StringRef(render_pass->name, sizeof(render_pass->name)).startswith(prefix)) {
          MultilayerColorOperation *op = new MultilayerColorOperation(
              render_layer, render_pass, view);
          op->setImage(image);
          op->setImageUser(iuser);
          op->setFramenumber(context.getFramenumber());
          r_input_operations.append(op);
        }
      }
    }
  }
  BKE_image_release_ibuf(image, ibuf, NULL);
}

blender::Vector<NodeOperation *> CryptomatteNode::createInputOperations(
    const CompositorContext &context, const bNode &node)
{
  blender::Vector<NodeOperation *> input_operations;
  switch (node.custom1) {
    case CMP_CRYPTOMATTE_SRC_RENDER:
      buildInputOperationsFromRenderSource(context, node, input_operations);
      break;
    case CMP_CRYPTOMATTE_SRC_IMAGE:
      buildInputOperationsFromImageSource(context, node, input_operations);
      break;
  }

  if (input_operations.is_empty()) {
    SetColorOperation *op = new SetColorOperation();
    op->setChannel1(0.0f);
    op->setChannel2(1.0f);
    op->setChannel3(0.0f);
    op->setChannel4(0.0f);
    input_operations.append(op);
  }
  return input_operations;
}

void CryptomatteNode::convertToOperations(NodeConverter &converter,
                                          const CompositorContext &context) const
{
  NodeInput *inputSocketImage = this->getInputSocket(0);
  NodeOutput *outputSocketImage = this->getOutputSocket(0);
  NodeOutput *outputSocketMatte = this->getOutputSocket(1);
  NodeOutput *outputSocketPick = this->getOutputSocket(2);

  bNode *node = this->getbNode();
  NodeCryptomatte *cryptoMatteSettings = (NodeCryptomatte *)node->storage;

  blender::Vector<NodeOperation *> input_operations = createInputOperations(context, *node);
  CryptomatteOperation *operation = new CryptomatteOperation(input_operations.size());
  LISTBASE_FOREACH (CryptomatteEntry *, cryptomatte_entry, &cryptoMatteSettings->entries) {
    operation->addObjectIndex(cryptomatte_entry->encoded_hash);
  }
  converter.addOperation(operation);
  for (int i = 0; i < input_operations.size(); ++i) {
    converter.addOperation(input_operations[i]);
    converter.addLink(input_operations[i]->getOutputSocket(), operation->getInputSocket(i));
  }

  SeparateChannelOperation *separateOperation = new SeparateChannelOperation;
  separateOperation->setChannel(3);
  converter.addOperation(separateOperation);

  SetAlphaMultiplyOperation *operationAlpha = new SetAlphaMultiplyOperation();
  converter.addOperation(operationAlpha);

  converter.addLink(operation->getOutputSocket(0), separateOperation->getInputSocket(0));
  converter.addLink(separateOperation->getOutputSocket(0), operationAlpha->getInputSocket(1));

  SetAlphaMultiplyOperation *clearAlphaOperation = new SetAlphaMultiplyOperation();
  converter.addOperation(clearAlphaOperation);
  converter.addInputValue(clearAlphaOperation->getInputSocket(1), 1.0f);

  converter.addLink(operation->getOutputSocket(0), clearAlphaOperation->getInputSocket(0));

  converter.mapInputSocket(inputSocketImage, operationAlpha->getInputSocket(0));
  converter.mapOutputSocket(outputSocketMatte, separateOperation->getOutputSocket(0));
  converter.mapOutputSocket(outputSocketImage, operationAlpha->getOutputSocket(0));
  converter.mapOutputSocket(outputSocketPick, clearAlphaOperation->getOutputSocket(0));
}
