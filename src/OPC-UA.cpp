/**
 * @brief 
 * 
 */

#include "OPC-UA.hpp"

OPCUA_Manager::OPCUA_Manager(const char* URL, const char* BaseID, OrderQueue *order_queue_reference, Warehouse *warehouse_reference) {
    strcpy(URL_, URL);
    client_ = ServerConnect(URL_);
    nodeIndex_ = 4;
    BaseNodeID_ = BaseID;
    if (!client_) {
        connected_ = false;
    }
    else {
        connected_ = true;
    }
    order_queue = order_queue_reference;
    warehouse = warehouse_reference;


    // fill pusher queues with dummy pieces. This gets rectified after the first CheckOutgoingPieces() call
    for (int i = 0; i < 3; i++){
        pusher_queue_size[i] = 4;
        pusher_queue[i].push(Order::Piece(0)); // that one piece that we never know if it's there or not
        for (int j = 0; j < 3; j++){
            machine_tools_in_use[i][j] = 1;
            piece_id_being_processed[i][j] = 0;
            last_piece_id_processed[i][j] = 0;
            pusher_queue[i].push(Order::Piece(0));
        }
    }
}

OPCUA_Manager::OPCUA_Manager(const char* URL, const char* BaseID, OrderQueue *order_queue_reference, Warehouse *warehouse_reference, uint16_t index) {
    strcpy(URL_, URL);
    client_ = ServerConnect(URL_);
    nodeIndex_ = index;
    BaseNodeID_ = BaseID;
    if (!client_) {
        connected_ = false;
    }
    else {
        connected_ = true;
    }
    order_queue = order_queue_reference;
    warehouse = warehouse_reference;


    // fill pusher queues with dummy pieces. This gets rectified after the first CheckOutgoingPieces() call
    for (int i = 0; i < 3; i++){
        pusher_queue_size[i] = 4;
        pusher_queue[i].push(Order::Piece(0)); // that one piece that we never know if it's there or not
        for (int j = 0; j < 3; j++){
            machine_tools_in_use[i][j] = 1;
            piece_id_being_processed[i][j] = 0;
            last_piece_id_processed[i][j] = 0;
            pusher_queue[i].push(Order::Piece(0));
        }
    }
}

// Completo: testa a conecao e tenta reconetar-se, se ja nao estiver conetado.
bool OPCUA_Manager::Reconnect(){
    // In case reconnect is called even though we were already connected
    if (client_) { 
        if (UA_Client_connect(client_, URL_) == UA_STATUSCODE_GOOD){
            return true;
        }
        UA_Client_delete(client_);
    }
    // Actual "reconnect" part:
    client_ = ServerConnect(URL_);
    if (client_) {
        return true;
    }
    else {
        return false;
    }
}

// Completo: reconecta para testar conecao
bool OPCUA_Manager::Is_Connected() {
    if (client_) {
        return (UA_Client_connect(client_, URL_) == UA_STATUSCODE_GOOD);
    }
    else {
        return false;
    }
}

// Completo: conecta ao servidor e retorna objeto (estrutura) client a ser usado por outras funcoes
UA_Client* OPCUA_Manager::ServerConnect(const char* endpointURL) const {
    UA_Client* client = UA_Client_new();
    UA_ClientConfig_setDefault(UA_Client_getConfig(client));

    UA_StatusCode retval = UA_Client_connect(client, endpointURL);
    if (retval != UA_STATUSCODE_GOOD) {
        UA_Client_delete(client);
        return NULL;
    }
    return client;
}

// Completo: pega em value e guarda-o numa string old fashioned C
void OPCUA_Manager::ConvIntToString(char* string, uint16_t value) {
    int aux = value, size, i;
    for (size = 1; aux > 9; size = size * 10) {
        aux = aux / 10;
    }

    for (i = 0; value > 9; i++) {
        string[i] = (value / size) + ASCII_NUMERAL_OFFSET;
        value = value - ((value / size) * size);
        size = size / 10;
    }
    string[i] = (value % 10) + ASCII_NUMERAL_OFFSET;
    string[i + 1] = 0;
}

// Completo: retorna true se nao houver pecas/transicao de pecas na carpete de saida de armazem
bool OPCUA_Manager::warehouseOutCarpetIsFree() {
    char NodeID[128];
    uint16_t mes_piece_tp;
    strcpy(NodeID, BaseNodeID_);
    strcat(NodeID, "PLC_PRG.AT1.MES_piece_tp"); // this variable will go to 0 after a piece has exited the warehouse carpet
    
    UA_Variant *val;
    UA_StatusCode retval;

    val = UA_Variant_new();
    retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
    if (val->type != &UA_TYPES[UA_TYPES_UINT16]){
        meslog(ERROR) << "Invalid node read! Should be type 16-bit unsigned integer!" << std::endl;
        return false;
    }
    if (retval != UA_STATUSCODE_GOOD){
        meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
        return false;
    }
    mes_piece_tp = *(UA_UInt16*)val->data;
    UA_Variant_delete(val);

    return (mes_piece_tp == 0);
}

// Envia ultima peca que esteja na lista de pecas da order. 
// Nesta altura a peca ja deve estar na base de dados e ter um id atribuido
bool OPCUA_Manager::SendPiece(Order::BaseOrder *order) {
    // Create base string for node access
    char NodeID[128];
    char NodeID_backup[128];
    char aux[20];
    strcpy(NodeID_backup, BaseNodeID_);
    strcat(NodeID_backup, "GVL.OBJECT[1].");

    // Check if we can insert the piece (Entry carpet is free)
    if (!warehouseOutCarpetIsFree()){
        meslog(ERROR) << "Can't send piece, warehouse carpet is occupied. Piece has been wrongly added to piece list!" << std::endl;
        return false; // warehouse carpet is not free, can't send piece
    }

    uint16_t pathIDcounter = 1;

    uint16_t object_index = 1;

    // Get data to send from order
    uint8_t pusher_destination = (order->GetType() == Order::ORDER_TYPE_UNLOAD ? order->GetFinalPiece() : 0);
    uint16_t type_piece = order->GetInitialPiece();
    uint8_t piece_type_to_remove = (uint8_t) type_piece;
    Order::Piece piece_copy = *(order->GetLastPiece());
    uint16_t id_piece = (uint16_t)piece_copy.GetID();
    uint8_t *moves = piece_copy.GetMoves();
    uint8_t *transformation = piece_copy.GetTransformations();
    uint8_t *machines = piece_copy.GetMachines();

    // Criar vetor em formato compatível com OPC-UA
    UA_Int16* path_UA = (UA_Int16*)UA_Array_new(59, &UA_TYPES[UA_TYPES_UINT16]);
    uint16_t i;
    for (i = 0; i < 59; i++) {
        path_UA[i] = (uint16_t) moves[i];
    }
    UA_Int16* transformation_UA = (UA_Int16*)UA_Array_new(59, &UA_TYPES[UA_TYPES_UINT16]);
    for (i = 0; i < 12; i++) {
        transformation_UA[i] = (uint16_t) transformation[i];
    }
    UA_Int16* machines_UA = (UA_Int16*)UA_Array_new(59, &UA_TYPES[UA_TYPES_UINT16]);
    for (i = 0; i < 9; i++) {
        machines_UA[i] = (uint16_t) machines[i];
    }

    // TESTING PENDING!!! Found out how to write in multiple places in a single write request, 
    // but this was found out by me (didn't see anyone else doing this), and might have unforeseen problems
    UA_WriteResponse wResp;
    UA_WriteRequest wReq;
    UA_WriteRequest_init(&wReq);
    wReq.nodesToWrite = UA_WriteValue_new();
    wReq.nodesToWriteSize = 1;
    // transformation node write
    strcpy(NodeID, NodeID_backup);
    strcat(NodeID, "transformation");
    wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
    wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
    wReq.nodesToWrite[0].value.hasValue = true;
    wReq.nodesToWrite[0].value.value.arrayLength = 12;
    wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
    wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
    wReq.nodesToWrite[0].value.value.data = transformation_UA;
    wResp = UA_Client_Service_write(client_, wReq);
    if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        return false;
    }
    UA_WriteRequest_clear(&wReq);
    UA_WriteResponse_clear(&wResp);

    // piece type node write
    UA_WriteRequest_init(&wReq);
    wReq.nodesToWrite = UA_WriteValue_new();
    wReq.nodesToWriteSize = 1;

    strcpy(NodeID, NodeID_backup);
    strcat(NodeID, "type_piece");
    wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
    wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
    wReq.nodesToWrite[0].value.hasValue = true;
    wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
    wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
    wReq.nodesToWrite[0].value.value.data = &type_piece;
    wResp = UA_Client_Service_write(client_, wReq);
    if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        return false;
    }
    UA_WriteRequest_clear(&wReq);
    UA_WriteResponse_clear(&wResp);

    // path_id_counter node write
    UA_WriteRequest_init(&wReq);
    wReq.nodesToWrite = UA_WriteValue_new();
    wReq.nodesToWriteSize = 1;
    strcpy(NodeID, NodeID_backup);
    strcat(NodeID, "path_id_counter");

    wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
    wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
    wReq.nodesToWrite[0].value.hasValue = true;
    wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
    wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
    wReq.nodesToWrite[0].value.value.data = &pathIDcounter;
    wResp = UA_Client_Service_write(client_, wReq);
    if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        return false;
    }
    UA_WriteRequest_clear(&wReq);
    UA_WriteResponse_clear(&wResp);

    // piece id node write
    UA_WriteRequest_init(&wReq);
    wReq.nodesToWrite = UA_WriteValue_new();
    wReq.nodesToWriteSize = 1;
    strcpy(NodeID, NodeID_backup);
    strcat(NodeID, "id_piece");

    wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
    wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
    wReq.nodesToWrite[0].value.hasValue = true;
    wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
    wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
    wReq.nodesToWrite[0].value.value.data = &id_piece;
    wResp = UA_Client_Service_write(client_, wReq);
    if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        return false;
    }
    UA_WriteRequest_clear(&wReq);
    UA_WriteResponse_clear(&wResp);

    // path node write
    UA_WriteRequest_init(&wReq);
    wReq.nodesToWrite = UA_WriteValue_new();
    wReq.nodesToWriteSize = 1;
    strcpy(NodeID, NodeID_backup);
    strcat(NodeID, "path");

    wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
    wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
    wReq.nodesToWrite[0].value.hasValue = true;
    wReq.nodesToWrite[0].value.value.arrayLength = 59;
    wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
    wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
    wReq.nodesToWrite[0].value.value.data = path_UA;
    wResp = UA_Client_Service_write(client_, wReq);
    if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        return false;
    }
    UA_WriteRequest_clear(&wReq);
    UA_WriteResponse_clear(&wResp);

    // machine transformations
    UA_WriteRequest_init(&wReq);
    wReq.nodesToWrite = UA_WriteValue_new();
    wReq.nodesToWriteSize = 1;
    strcpy(NodeID, NodeID_backup);
    strcat(NodeID, "machine_transformations");

    wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
    wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
    wReq.nodesToWrite[0].value.hasValue = true;
    wReq.nodesToWrite[0].value.value.arrayLength = 9;
    wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
    wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
    wReq.nodesToWrite[0].value.value.data = machines_UA;
    wResp = UA_Client_Service_write(client_, wReq);
    if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        return false;
    }
    UA_WriteRequest_clear(&wReq);
    UA_WriteResponse_clear(&wResp);

    // Escrever tipo de peca na variavel da Warehouse para despoletar a saida da peca.

    UA_WriteRequest_init(&wReq);
    wReq.nodesToWrite = UA_WriteValue_new();
    wReq.nodesToWriteSize = 1;
    strcpy(NodeID, BaseNodeID_);
    strcat(NodeID, "PLC_PRG.AT1.MES_piece_tp");
    wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
    wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
    wReq.nodesToWrite[0].value.hasValue = true;
    wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
    wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
    wReq.nodesToWrite[0].value.value.data = &type_piece;
    wResp = UA_Client_Service_write(client_, wReq);
    if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        return false;
    }
    UA_WriteRequest_clear(&wReq);
    UA_WriteResponse_clear(&wResp);

    // remove piece from warehouse
    warehouse->RemovePiece(piece_type_to_remove);
    // push piece to queue, reserving a spot in a slider and check it as finished in DataBase
    if (pusher_destination > 0){ // same as checking if it's an unload order
        if (pusher_destination > 3){
            meslog(ERROR) << "Invalid pusher destination. Piece will still be sent and updated but not included in pusher allocation." << std::endl;
            order_queue->RemovePiece(piece_copy.GetID()); // imediately check piece as finished
        }
        pusher_queue[pusher_destination-1].push(piece_copy);
        if (pusher_queue[pusher_destination-1].size() > 4){
            meslog(ERROR) << "Pusher " << (int)pusher_destination << " has " << pusher_queue[pusher_destination-1].size() - 4 << " excess pieces allocated to it! This is only an ERROR if pathfinder already has this feature." << std::endl;
        }
        order_queue->RemovePiece(piece_copy.GetID()); // imediately check piece as finished
    }

    return true;
}

// Retorna true se conseguiu ler pecas, ou false se nao conseguiu (porque nao havia ou porque falhou)
bool OPCUA_Manager::CheckPiecesFinished(){
    char NodeID[128];
    char NodeID_backup[128];
    char aux[3];
    uint16_t number_of_ids_to_read;
    bool all_up_to_date = true;

    strcpy (NodeID_backup, BaseNodeID_);
    strcat (NodeID_backup, "PLC_PRG.AT2.piece_queue[");
    

    // Check if there are true booleans in warehouse entry carpet (pieces that the warehouse 
    // received but haven't yet been processed by MES)
    bool piece_queue[10] = { false };
    uint16_t piece_ids[10] = {0};
    uint8_t piece_type_to_add;
    UA_Variant *val;
    UA_StatusCode retval;

    uint16_t i;

    for (i = 0; i < 10; i++) {
        strcpy(NodeID, NodeID_backup);
        ConvIntToString(aux, i + 1); //PLC array indexes start at 1
        strcat(NodeID, aux);
        strcat(NodeID, "]");

        val = UA_Variant_new();
        retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
        if (val->type != &UA_TYPES[UA_TYPES_BOOLEAN]){
            meslog(ERROR) << "Invalid node read! Should be type boolean!" << std::endl;
            return false;
        }
        if (retval != UA_STATUSCODE_GOOD){
            meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
            return false;
        }
        piece_queue[i] = *(UA_Boolean*)val->data;
        UA_Variant_delete(val);
    }
    // bool results are now in piece_queue[10];

    
    strcpy (NodeID_backup, BaseNodeID_);
    strcat (NodeID_backup, "PLC_PRG.AT2.piece_id_array[");

    // check if the queue has any unprocessed pieces and count how many nodes we need to read
    number_of_ids_to_read = 0;
    for (i = 0; i < 10; i++){
        if (piece_queue[i]){
            all_up_to_date = false; // at least one element is true -> not all is up to date

            strcpy(NodeID, NodeID_backup);
            ConvIntToString(aux, i + 1); //PLC array indexes start at 1
            strcat(NodeID, aux);
            strcat(NodeID, "]");

            val = UA_Variant_new();
            retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
            if (val->type != &UA_TYPES[UA_TYPES_UINT16]){
                meslog(ERROR) << "Invalid node read! Should be type 16-bit unsigned integer!" << std::endl;
                return false;
            }
            if (retval != UA_STATUSCODE_GOOD){
                meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
                return false;
            }
            piece_ids[i] = *(UA_UInt16*)val->data;
            UA_Variant_delete(val);

            piece_type_to_add = order_queue->RemovePiece((uint32_t) piece_ids[i]);
            warehouse->RemovePiece(piece_type_to_add);

            number_of_ids_to_read++;
        }
    }

    if (all_up_to_date){
        return false; // nothing to do, return "false" meaning no pieces were finished
    }

    meslog(INFO) << "There were " << number_of_ids_to_read << " unprocessed pieces in the warehouse buffer." << std::endl;

    // With everything done, we can write "false" in the piece queue, meaning we've processed that piece and the PLC can reuse that buffer position.
    // We don't need to overwrite the old ids, they will just be overwritten later on by the PLC upon
    // checking that the piece_queue index that corresponds to it is "false".

    uint16_t node_index = 0;
    UA_WriteRequest wReq;
    UA_WriteResponse wResp;
    bool write_false = false; // value to write in the piece_queue: is always false. Just a quirk of how open62541 works

    strcpy (NodeID_backup, BaseNodeID_);
    strcat (NodeID_backup, "PLC_PRG.AT2.piece_queue[");

    // fill in which nodes to write to
    // we could simply write to all nodes in the queue, but we risk that the PLC writes to a node while this function
    // executes and then we'd miss that read, and a piece would disappear from the MES (as if it would be stuck forever 
    // in the factory floor)
    for (i = 0; i < 10; i++){
        if (piece_queue[i]){

            strcpy(NodeID, NodeID_backup);
            ConvIntToString(aux, i + 1); // PLC array indexes start at 1
            strcat(NodeID, aux);
            strcat(NodeID, "]");

            UA_WriteRequest_init(&wReq);
            wReq.nodesToWrite = UA_WriteValue_new();
            wReq.nodesToWriteSize = 1;
            wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
            wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
            wReq.nodesToWrite[0].value.hasValue = true;
            wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_BOOLEAN];
            wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
            wReq.nodesToWrite[0].value.value.data = &write_false;
            wResp = UA_Client_Service_write(client_, wReq);
            if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
                meslog(ERROR) << "Invalid node write! Server responded with ERROR!" << std::endl;
                return false;
            }
            UA_WriteRequest_clear(&wReq);
            UA_WriteResponse_clear(&wResp);
        }
    }

    return true;
}

// Completo: verifica pecas que estejam nos tapetes de carga. Se houver pecas adiciona-as
// imediatamente a uma order que tambem e criada pela funcao. Esta funcao tambem
// serve para o PLC poder prosseguir e colocar a peca acabada de chegar no armazem.
// Retorna true se tiver detetado pecas (em qualquer um dos tapetes) e false se ambos
// os tapetes estavam vazios
bool OPCUA_Manager::CheckIncomingPieces(){
    // to write stuff with OPC_UA (might go unused)
    UA_WriteRequest wReq;
    UA_WriteResponse wResp;
    uint16_t PieceID;
    bool Mes_is_ok = true;
    bool MES_ok = true;
    bool piece_present;

    bool return_value = false;

    ////////////////////////////////////////////////////////////// CARPET C7T1B
    char NodeID[128];
    
    // Check if MES ok variable in PLC is not set (we haven't processed this piece yet)
    strcpy (NodeID,BaseNodeID_);
    strcat (NodeID,"PLC_PRG.C7T1b.piece_p");

    UA_Variant *val = UA_Variant_new();
    UA_StatusCode retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
    if(retval == UA_STATUSCODE_GOOD) {
        piece_present = *(UA_Boolean*)val->data;
    }
    UA_Variant_delete(val);

    if (piece_present){
        strcpy (NodeID,BaseNodeID_);
        strcat (NodeID,"PLC_PRG.C7T1b.MES_ok");

        val = UA_Variant_new();
        retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
        if(retval == UA_STATUSCODE_GOOD) {
            MES_ok = *(UA_Boolean*)val->data;
        }
        UA_Variant_delete(val);
    }
    // MES ok is false: we have not yet processed this piece
    if (!MES_ok){
        PieceID = (uint16_t) order_queue->AddOrder(Order::BaseOrder(0, Order::ORDER_TYPE_LOAD, 1, 1, 1, "0")); //deadline não interessa, pus "0" just in case

        strcpy(NodeID, BaseNodeID_);
        strcat(NodeID, "GVL.OBJECT[9].id_piece");

        UA_WriteRequest_init(&wReq);
        wReq.nodesToWrite = UA_WriteValue_new();
        wReq.nodesToWriteSize = 1;
        wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
        wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
        wReq.nodesToWrite[0].value.hasValue = true;
        wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
        wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
        wReq.nodesToWrite[0].value.value.data = &PieceID;
        wResp = UA_Client_Service_write(client_, wReq);
        if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
            meslog(ERROR) << "Invalid node write to OBJECT[9].id_piece! Server responded with ERROR!" << std::endl;
            return false;
        }
        UA_WriteRequest_clear(&wReq);
        UA_WriteResponse_clear(&wResp);

        
        strcpy(NodeID, BaseNodeID_);
        strcat(NodeID, "PLC_PRG.C7T1b.MES_ok");

        UA_WriteRequest_init(&wReq);
        wReq.nodesToWrite = UA_WriteValue_new();
        wReq.nodesToWriteSize = 1;
        wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
        wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
        wReq.nodesToWrite[0].value.hasValue = true;
        wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
        wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
        wReq.nodesToWrite[0].value.value.data = &PieceID;
        wResp = UA_Client_Service_write(client_, wReq);
        if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
            meslog(ERROR) << "Invalid node write to MES_ok! Server responded with ERROR!" << std::endl;
            return false;
        }
        UA_WriteRequest_clear(&wReq);
        UA_WriteResponse_clear(&wResp);

        meslog(INFO) << "Received piece P1 on carpet C7T1b" << std::endl;
        return_value = true;
    }
    MES_ok = true;

    ////////////////////////////////////////////////////////////// CARPET C7T7B
    // Check if MES ok variable in PLC is not set (we haven't processed this piece yet)
    strcpy (NodeID,BaseNodeID_);
    strcat (NodeID,"PLC_PRG.C7T7b.piece_p");

    val = UA_Variant_new();
    retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
    if(retval == UA_STATUSCODE_GOOD) {
        piece_present = *(UA_Boolean*)val->data;
    }
    UA_Variant_delete(val);

    if (piece_present){
        strcpy (NodeID,BaseNodeID_);
        strcat (NodeID,"PLC_PRG.C7T7b.MES_ok");

        val = UA_Variant_new();
        retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
        if(retval == UA_STATUSCODE_GOOD) {
            MES_ok = *(UA_Boolean*)val->data;
        }
        UA_Variant_delete(val);
    }

    // MES ok is false: we have not yet processed this piece
    if (!MES_ok){
        // Adicionar Order antes de mandar o MES_ok
        PieceID = (uint16_t) order_queue->AddOrder(Order::BaseOrder(0, Order::ORDER_TYPE_LOAD, 1, 2, 2, "0")); //deadline não interessa, pus "0" just in case

        strcpy(NodeID, BaseNodeID_);
        strcat(NodeID, "GVL.OBJECT[59].id_piece");

        UA_WriteRequest_init(&wReq);
        wReq.nodesToWrite = UA_WriteValue_new();
        wReq.nodesToWriteSize = 1;
        wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
        wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
        wReq.nodesToWrite[0].value.hasValue = true;
        wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
        wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
        wReq.nodesToWrite[0].value.value.data = &PieceID;
        wResp = UA_Client_Service_write(client_, wReq);
        if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
            meslog(ERROR) << "Invalid node write to OBJECT[59].id_piece! Server responded with ERROR!" << std::endl;
            return false;
        }
        UA_WriteRequest_clear(&wReq);
        UA_WriteResponse_clear(&wResp);
        

        strcpy(NodeID, BaseNodeID_);
        strcat(NodeID, "PLC_PRG.C7T7b.MES_ok");

        UA_WriteRequest_init(&wReq);
        wReq.nodesToWrite = UA_WriteValue_new();
        wReq.nodesToWriteSize = 1;
        wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
        wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
        wReq.nodesToWrite[0].value.hasValue = true;
        wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_BOOLEAN];
        wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
        wReq.nodesToWrite[0].value.value.data = &Mes_is_ok;
        wResp = UA_Client_Service_write(client_, wReq);
        if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
            meslog(ERROR) << "Invalid node write to MES_ok! Server responded with ERROR!" << std::endl;
            return false;
        }
        UA_WriteRequest_clear(&wReq);
        UA_WriteResponse_clear(&wResp);

        meslog(INFO) << "Received piece P2 on carpet C7T7b" << std::endl;
        return_value = true;
    }


    return return_value;
}

bool OPCUA_Manager::CheckOutgoingPieces(){
    // OPC-UA Node Read
    
    UA_WriteRequest wReq;
    UA_WriteResponse wRep;
    UA_StatusCode retval;
    UA_Variant *val;
    char NodeID[128] = {0};
    char NodeID_Backup[128];
    char NodeID_Backup2[128];
    char NodeID_Backup3[128];
    char aux [5];
    uint16_t queue_size_in_PLC[3];
    bool piece_pusher_queue[3][10];
    uint16_t piece_pusher_ids[3][10];
    bool flag_write = false;
    bool return_value = false;
    int pusher, buffer_index;

    strcpy (NodeID_Backup, BaseNodeID_);
    strcat (NodeID_Backup,"PLC_PRG.C7T");

    for (pusher = 0; pusher < 3; pusher++){
        strcpy (NodeID_Backup2, NodeID_Backup);

        switch(i) {
            case 0:
                strcat (NodeID_Backup2, "3");
                break;
            case 1:
                strcat (NodeID_Backup2, "4");
                break;
            case 2:
                strcat (NodeID_Backup2, "5");
                break;
        }

        // Read stuff for pusher queue allocation (not piece-specific stuff)
        strcpy (NodeID_Backup3, NodeID_Backup2);
        strcpy (NodeID, NodeID_Backup3);
        strcat (NodeID, ".slider_queue");

        val = UA_Variant_new();
        retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
        if (val->type != &UA_TYPES[UA_TYPES_UINT16]){
            meslog(ERROR) << "Invalid node read! Should be type 16-bit unsigned integer!" << std::endl;
            return false;
        }
        if (retval != UA_STATUSCODE_GOOD){
            meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
            return false;
        }

        queue_size_in_PLC[pusher] = *(UA_UInt16*)val->data;
        UA_Variant_delete(val);

        // Read piece ids in queue to find out which pieces evacuated, i.e. are in sliders
        
        
        strcpy (NodeID, NodeID_Backup3);
        strcat (NodeID, ".pushed_piece_queue[");

        for (buffer_index = 0; buffer_index < 10; buffer_index++){
            strcpy (NodeID, NodeID_Backup3);
            ConvIntToString(aux, (uint16_t) buffer_index+1));
            strcat (NodeID, aux);
            strcat (NodeID, "]");
            val = UA_Variant_new();
            retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
            if (val->type != &UA_TYPES[UA_TYPES_BOOLEAN]){
                meslog(ERROR) << "Invalid node read! Should be type boolean!" << std::endl;
                return false;
            }
            if (retval != UA_STATUSCODE_GOOD){
                meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
                return false;
            }

            piece_pusher_queue[pusher][buffer_index] = *(UA_Boolean*)val->data;
            UA_Variant_delete(val);

            if (piece_pusher_queue[pusher][buffer_index]){
                strcpy (NodeID, NodeID_Backup3);
                ConvIntToString(aux, (uint16_t) buffer_index+1));
                strcat (NodeID, aux);
                strcat (NodeID, "]");
                val = UA_Variant_new();
                retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
                if (val->type != &UA_TYPES[UA_TYPES_UINT16]){
                    meslog(ERROR) << "Invalid node read! Should be type 16-bit unsigned integer!!" << std::endl;
                    return false;
                }
                if (retval != UA_STATUSCODE_GOOD){
                    meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
                    return false;
                }

                piece_pusher_ids[pusher][buffer_index] = *(UA_UInt16*)val->data;
                UA_Variant_delete(val);

                
                strcpy (NodeID, NodeID_Backup2);
                strcat (NodeID, ".pushed_piece_queue[");
                ConvIntToString(aux, (uint16_t) buffer_index+1));
                strcat (NodeID, aux);
                strcat (NodeID, "]");

                UA_WriteRequest_init(&wReq);
                wReq.nodesToWrite = UA_WriteValue_new();
                wReq.nodesToWriteSize = 1;
                wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
                wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
                wReq.nodesToWrite[0].value.hasValue = true;
                wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
                wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
                wReq.nodesToWrite[0].value.value.data = &flag_write;
                wResp = UA_Client_Service_write(client_, wReq);
                if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
                    meslog(ERROR) << "Invalid node write to Pusher id queue flag! Server responded with ERROR!" << std::endl;
                    return false;
                }
                UA_WriteRequest_clear(&wReq);
                UA_WriteResponse_clear(&wResp);
            }
        }

        strcpy (NodeID_Backup3, NodeID_Backup2);
        strcat (NodeID_Backup3, ".pushed_piece_id_array[");

        for (buffer_index = 0; buffer_index < 10; buffer_index++){
            if (piece_pusher_queue[pusher][buffer_index]){
                strcpy (NodeID, NodeID_Backup3);
                ConvIntToString(aux, (uint16_t) buffer_index+1));
                strcat (NodeID, aux);
                strcat (NodeID, "]");
                val = UA_Variant_new();
                retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
                if (val->type != &UA_TYPES[UA_TYPES_UINT16]){
                    meslog(ERROR) << "Invalid node read! Should be type 16-bit unsigned integer!!" << std::endl;
                    return false;
                }
                if (retval != UA_STATUSCODE_GOOD){
                    meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
                    return false;
                }

                piece_pusher_ids[pusher][buffer_index] = *(UA_UInt16*)val->data;
                UA_Variant_delete(val);

                
                strcpy (NodeID, NodeID_Backup2);
                strcat (NodeID, ".pushed_piece_queue[");
                ConvIntToString(aux, (uint16_t) buffer_index+1));
                strcat (NodeID, aux);
                strcat (NodeID, "]");

                UA_WriteRequest_init(&wReq);
                wReq.nodesToWrite = UA_WriteValue_new();
                wReq.nodesToWriteSize = 1;
                wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(nodeIndex_, NodeID);
                wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
                wReq.nodesToWrite[0].value.hasValue = true;
                wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_UINT16];
                wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE;
                wReq.nodesToWrite[0].value.value.data = &flag_write;
                wResp = UA_Client_Service_write(client_, wReq);
                if (wResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
                    meslog(ERROR) << "Invalid node write to Pusher id queue flag! Server responded with ERROR!" << std::endl;
                    return false;
                }
                UA_WriteRequest_clear(&wReq);
                UA_WriteResponse_clear(&wResp);
            }
        }
    }

    /// OPC-UA Node Read finished. All pusher queue sizes and unloaded pieces in PLC have been read.

    // Update Pusher allocation
    for (pusher = 0; pusher < 3; pusher++){
        while (queue_size_in_PLC[i] < pusher_queue_size[pusher]){
            meslog(INFO) << "Popped piece " << pusher_queue[pusher].front().GetID() << " from Pusher " << pusher+1 << "'s queue." << std::endl;
            return_value = true;
            pusher_queue[pusher].pop();
            pusher_queue_size[pusher] -= 1;
        }pusher_queue_size[pusher] = queue_size_in_PLC[pusher];
    }

    // Update DB
    for (pusher = 0; pusher < 3; pusher++){
        for (buffer_index = 0; buffer_index < 10; buffer_index++){
            if (piece_pusher_queue[pusher][buffer_index]){
                updateDataPiece(DBFILE, (int) piece_pusher_ids[pusher][buffer_index]);
            }
        }
    }

    return return_value;
}

void OPCUA_Manager::UpdateMachineInfo(){
    UA_StatusCode retval;
    UA_Variant *val;
    bool tool_changed = false;
    char NodeID[128] = {0};
    char NodeID_Backup[128];
    char NodeID_Backup2[128];
    char NodeID_Backup3[128];
    char aux[3];
    char machine_type_name;

    strcpy(NodeID_Backup, BaseNodeID_);
    strcat(NodeID_Backup, "PLC_PRG.C");

    for (int cell = 0; cell < 3; cell++){
        strcpy (NodeID_Backup2, NodeID_Backup);
        ConvIntToString(aux, 1+cell*2);
        strcat (NodeID_Backup2, aux);
        strcat (NodeID_Backup2, "T");

        for (int machine_type = 0; machine_type < 3; machine_type++){
            strcpy (NodeID_Backup3, NodeID_Backup2);
            ConvIntToString(aux, machine_type+3);
            strcat(NodeID_Backup3, aux);
            strcpy(NodeID, NodeID_Backup3);
            
            strcat(NodeID, ".requested_tool");

            val = UA_Variant_new();
            retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
            if (val->type != &UA_TYPES[UA_TYPES_UINT16]){
                meslog(ERROR) << "Invalid node read! Should be type 16-bit unsigned integer!" << std::endl;
                UA_Variant_delete(val);
                return;
            }
            if (retval != UA_STATUSCODE_GOOD){
                meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
                UA_Variant_delete(val);
                return;
            }
            machine_tools_in_use[cell][machine_type] = (uint8_t)(*(UA_UInt16*)val->data);
            UA_Variant_delete(val);
            
            strcpy(NodeID, NodeID_Backup3);
            strcat(NodeID, ".last_processed_piece_id");

            val = UA_Variant_new();
            retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
            if (val->type != &UA_TYPES[UA_TYPES_UINT16]){
                meslog(ERROR) << "Invalid node read! Should be type 16-bit unsigned integer!" << std::endl;
                UA_Variant_delete(val);
                return;
            }
            if (retval != UA_STATUSCODE_GOOD){
                meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
                UA_Variant_delete(val);
                return;
            }
            last_piece_id_processed[cell][machine_type] = *(UA_UInt16*)val->data;
            UA_Variant_delete(val);
            
            strcpy(NodeID, NodeID_Backup3);
            strcat(NodeID, ".piece_being_processed_id");

            val = UA_Variant_new();
            retval = UA_Client_readValueAttribute(client_, UA_NODEID_STRING(nodeIndex_, NodeID), val);
            if (val->type != &UA_TYPES[UA_TYPES_UINT16]){
                meslog(ERROR) << "Invalid node read! Should be type 16-bit unsigned integer!" << std::endl;
                UA_Variant_delete(val);
                return;
            }
            if (retval != UA_STATUSCODE_GOOD){
                meslog(ERROR) << "Invalid node read! Server responded with ERROR!" << std::endl;
                UA_Variant_delete(val);
                return;
            }
            piece_id_being_processed[cell][machine_type] = *(UA_UInt16*)val->data;
            UA_Variant_delete(val);
        }
    }
}

void OPCUA_Manager::UpdateMachineProcessedTime(){
    
}

uint16_t OPCUA_Manager::GetPieceAllocInPusher(uint8_t pusher_number){
    if (pusher_number > 3 || pusher_number < 1){
        meslog(ERROR) << "Attempted to use invalid pusher identifier! Identify pushers by 1, 2 or 3." << std::endl;
        return 0;
    }
    return pusher_queue[pusher_number-1].size();

}

uint16_t OPCUA_Manager::GetCurrentToolInMachine(uint8_t machine_type, uint8_t cell_number){
    if (cell_number > 3 || cell_number < 1){
        meslog(ERROR) << "Attempted to use invalid pusher identifier! Identify cells by 1, 2 or 3." << std::endl;
        return 0;
    }
    
    if (machine_type > 3 || machine_type < 1){
        meslog(ERROR) << "Attempted to use invalid pusher identifier! Identify machines by 1, 2 or 3, for A, B and C respectively." << std::endl;
        return 0;
    }
    return (uint16_t) machine_tools_in_use[cell_number][machine_type];
}

uint16_t OPCUA_Manager::GetCurrentPieceIDInMachine(uint8_t machine_type, uint8_t cell_number){
    if (cell_number > 3 || cell_number < 1){
        meslog(ERROR) << "Attempted to use invalid pusher identifier! Identify cells by 1, 2 or 3." << std::endl;
        return 0;
    }
    
    if (machine_type > 3 || machine_type < 1){
        meslog(ERROR) << "Attempted to use invalid pusher identifier! Identify machines by 1, 2 or 3, for A, B and C respectively." << std::endl;
        return 0;
    }
    return piece_id_being_processed[cell_number][machine_type];
}

uint16_t OPCUA_Manager::GetLastMadePieceIDInMachine(uint8_t machine_type, uint8_t cell_number){
    if (cell_number > 3 || cell_number < 1){
        meslog(ERROR) << "Attempted to use invalid pusher identifier! Identify cells by 1, 2 or 3." << std::endl;
        return 0;
    }
    
    if (machine_type > 3 || machine_type < 1){
        meslog(ERROR) << "Attempted to use invalid pusher identifier! Identify machines by 1, 2 or 3, for A, B and C respectively." << std::endl;
        return 0;
    }
    return last_piece_id_processed[cell_number][machine_type];
}










void OPCUA_Manager::print_all_machine_info(){
    std::cout << "\t>>>MACHINE INFO<<<" << std::endl;
    
    std::cout << ">>>CELL 1: " << std::endl;
    std::cout << "Machine A |||\tTool: " << (int) machine_tools_in_use[0][0] << "\t| Last Piece ID: " << last_piece_id_processed[0][0] << "\t| Current Piece ID: " << piece_id_being_processed[0][0] << std::endl;
    std::cout << "Machine B |||\tTool: " << (int) machine_tools_in_use[0][1] << "\t| Last Piece ID: " << last_piece_id_processed[0][1] << "\t| Current Piece ID: " << piece_id_being_processed[0][1] << std::endl;
    std::cout << "Machine C |||\tTool: " << (int) machine_tools_in_use[0][2] << "\t| Last Piece ID: " << last_piece_id_processed[0][2] << "\t| Current Piece ID: " << piece_id_being_processed[0][2] << std::endl;
    std::cout << "---------------------------------------------------------------------------------------------------------------------" << std::endl;
    std::cout << ">>>CELL 2: " << std::endl;
    std::cout << "Machine A |||\tTool: " << (int) machine_tools_in_use[1][0] << "\t| Last Piece ID: " << last_piece_id_processed[1][0] << "\t| Current Piece ID: " << piece_id_being_processed[1][0] << std::endl;
    std::cout << "Machine B |||\tTool: " << (int) machine_tools_in_use[1][1] << "\t| Last Piece ID: " << last_piece_id_processed[1][1] << "\t| Current Piece ID: " << piece_id_being_processed[1][1] << std::endl;
    std::cout << "Machine C |||\tTool: " << (int) machine_tools_in_use[1][2] << "\t| Last Piece ID: " << last_piece_id_processed[1][2] << "\t| Current Piece ID: " << piece_id_being_processed[1][2] << std::endl;
    std::cout << "---------------------------------------------------------------------------------------------------------------------" << std::endl;
    std::cout << ">>>CELL 3: " << std::endl;
    std::cout << "Machine A |||\tTool: " << (int) machine_tools_in_use[2][0] << "\t| Last Piece ID: " << last_piece_id_processed[2][0] << "\t| Current Piece ID: " << piece_id_being_processed[2][0] << std::endl;
    std::cout << "Machine B |||\tTool: " << (int) machine_tools_in_use[2][1] << "\t| Last Piece ID: " << last_piece_id_processed[2][1] << "\t| Current Piece ID: " << piece_id_being_processed[2][1] << std::endl;
    std::cout << "Machine C |||\tTool: " << (int) machine_tools_in_use[2][2] << "\t| Last Piece ID: " << last_piece_id_processed[2][2] << "\t| Current Piece ID: " << piece_id_being_processed[2][2] << std::endl;
}